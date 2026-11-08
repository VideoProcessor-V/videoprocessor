// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// The typed interface P5a (the vector-engine extension) must expose to the camera link (doc 56 section 0, F1).
// Query id "vpv.vector_engine.v1". Header only, no implementation here: P5a implements IVectorEngine; the camera link
// (global_model/link) and the tests use a fake. Everything is read-only for the consumer except the pre-estimate
// hook, which is the ONE place the camera link reaches the matcher (doc 52 3.1 f; maps 1:1 to
// MotionWarp::SetCameraGuidance and CameraGuidanceCounts).
#pragma once

#include <VP-V/core/Indicator.h>
#include <VP-V/modules/motion/estimation/global_model/CameraGuidanceGpu.h>

#include <libplacebo/gpu.h>

#include <cstdint>
#include <memory>

namespace VpMotion
{
	// Query("vpv.vector_engine.v1") answers an IVectorEngineDirectory* (below): the hub's query carries no host and an
	// engine is per renderer, so the consumer asks the directory for its host's engine (M2 P5a).
	constexpr const char* kVectorEngineQueryId = "vpv.vector_engine.v1";

	// Required texture capabilities: fwd, bwd and the counts texture must be SAMPLEABLE rgba32f (the camera readback copy pass
	// reads them with texelFetch); they need not be host readable. Threading (M2 P5a-5): BeforeEstimate / AfterEstimate are
	// called for BOTH ahead == false (the in-render estimate) and ahead == true (the de-judder's estimate ahead), and today
	// both run on the render thread with renderMutex held, so GuidanceScheduler::Guide may be called from either. A future
	// estimate on an off-render lane thread must NOT call Guide from the hook (PublishLane covers that path from the camera
	// worker); SetPreEstimateHook (set and clear) runs on the render thread or under renderMutex.
	// A verified analysis field (P3) of one source pair. Textures are owned by the engine: valid only while the
	// field's seq is within the engine's ring (the consumer copies, never keeps the pl_tex).
	struct VerifiedField
	{
		uint64_t seq = 0, pairFrom = 0, pairTo = 0;
		pl_tex fwd = nullptr;        // rgba32f (vx, vy, trust, unused) per block, MATCHER px (scaled by scaleToSourcePx)
		pl_tex bwd = nullptr;        // same layout, may be null (inverse signal then neutral)
		pl_tex trust = nullptr;      // optional separate trust plane, may be null
		int gridW = 0, gridH = 0, blockSize = 0;   // blockSize in source px
		float scaleToSourcePx = 1.0f;              // multiply a field vector by this to get source px
		VpIndicator::Stamp stamp;                  // the stamp the field was computed under
	};

	struct GlobalVec
	{
		bool valid = false;
		double x = 0, y = 0;         // source px over the pair
	};

	// A CPU-resident flow view (for temporalSeed); null planes when the flow is GPU-only.
	struct FlowView
	{
		const float* x = nullptr;
		const float* y = nullptr;
		int gridW = 0, gridH = 0;
		double blockSize = 0;
		uint64_t from = 0, to = 0;
	};

	class IPreEstimateHook;

	// ---- RESERVATIONS (doc 72 item 7; M2 P5a): entry points and contracts only, no implementation. Both are
	// "absent" placeholders: a default implementation says so, and a caller falls back VISIBLY (a fallback entry
	// naming it, never a silent downgrade). The implementations belong to the camera + motion lead (doc 70).
	// BDOF / PROF per-pixel offsets are NOT stored by the engine (dropped).

	// (a) P3r refinement stage: product `refined_field`, the U15 FieldRefine stage (DMVR-style per-block refinement
	// around the engine's vectors). A lane stage after P3; it never starts the matcher. Absent: consumers read the P3
	// verified field (FieldOf) and say so.
	constexpr const char* kRefinedFieldProduct = "refined_field";
	constexpr const char* kRefinedFieldFallbackId = "vector_engine_refined_field";

	// (b) The async LONG-PAIR service: a pair of NON-adjacent source frames (a gap, a pulldown span, a camera fit's
	// baseline) estimated by the NVIDIA optical-flow engine WITH EXTERNAL HINTS (today NvOpticalFlow opens with
	// enableExternalHints = NV_OF_FALSE; the service will open a second session with hints on). The hint is a
	// CPU-resident field in the same units as FlowView (block-grid vectors in source pixels).
	struct HintField
	{
		const float* x = nullptr;
		const float* y = nullptr;
		int gridW = 0, gridH = 0;
		double blockSize = 0;              // source px per block
		uint64_t from = 0, to = 0;         // the pair the hint was made for
	};
	enum class LongPairResult
	{
		Unavailable,   // the placeholder answer: no service in this build / engine (the caller falls back, visibly)
		Accepted,      // queued; poll with the ticket
		Rejected,      // refused (bad arguments, queue full): the caller falls back, visibly
	};
	constexpr const char* kLongPairFallbackId = "vector_engine_long_pair";
	// What the fallback means, in plain words (the status text of the entry the caller raises).
	constexpr const char* kLongPairFallbackText =
		"long-pair optical flow with hints is not available (the matcher's seeded search estimates the pair instead)";

	class IVectorEngine
	{
	public:
		virtual ~IVectorEngine() {}
		// The newest verified field's seq (0 = none). Monotonic within a stamp era: the "new field" signal.
		virtual uint64_t LatestVerifiedFieldSeq() const = 0;
		// Changes whenever the numbering of the engine fields restarts (seek, resync, graph rebuild, stamp era change). The camera
		// link forgets which fields it already requested when it changes.
		virtual uint64_t FieldEra() const = 0;
		virtual bool FieldOf(uint64_t seq, VerifiedField& out) const = 0;
		virtual float FieldScale() const = 0;
		virtual bool GlobalFromEngine() const = 0;
		virtual bool EngineGlobal(uint64_t seq, GlobalVec& out) const = 0;
		virtual bool PairSpansSlip(uint64_t from, uint64_t to) const = 0;
		virtual bool FlowIsCpuResident() const = 0;
		virtual bool FlowOf(uint64_t seq, FlowView& out) const = 0;
		// The camera link registers its hook here (null = none). One hook per engine.
		virtual void SetPreEstimateHook(IPreEstimateHook* hook) = 0;

		// ---- reservations (see above); defaults are the "absent" placeholders, so existing implementers and fakes
		// need no change. Threading when implemented: SubmitLongPair is called from the engine lane thread (never the
		// render thread, never under renderMutex) and returns at once; the service computes on its own thread;
		// PollLongPair is lock-free for the caller. The textures passed to SubmitLongPair must stay valid until
		// PollLongPair reports completion or CancelLongPair returns; the engine never keeps them beyond that. A
		// completed pair is a VerifiedField whose stamp is the pair's stamp era (stale-stamp results are dropped).
		// (a) the refined field of seq (P3r / U15); false = absent.
		virtual bool RefinedFieldOf(uint64_t /*seq*/, VerifiedField& /*out*/) const { return false; }
		virtual bool RefinedFieldAvailable() const { return false; }
		// (b) the long-pair service.
		virtual LongPairResult SubmitLongPair(pl_tex /*lumaA*/, pl_tex /*lumaB*/, const HintField* /*hintField*/,
			uint64_t /*pairFrom*/, uint64_t /*pairTo*/, uint64_t& ticket)
		{
			ticket = 0;
			return LongPairResult::Unavailable;
		}
		virtual bool PollLongPair(uint64_t /*ticket*/, VerifiedField& /*out*/) { return false; }
		virtual void CancelLongPair(uint64_t /*ticket*/) {}
	};

	// What Query(kVectorEngineQueryId) returns. EngineFor(host) = the engine of that renderer (host = the pointer the
	// renderer passes as RendererExtension Context::host), or null when that renderer has none (not created yet,
	// destroyed). Any thread (a mutex, never taken per frame by the engine). The pointer is valid until that renderer
	// is destroyed: a consumer drops it at the host's GpuReleased / Shutdown and calls SetPreEstimateHook(nullptr)
	// before its hook object goes.
	class IVectorEngineDirectory
	{
	public:
		virtual ~IVectorEngineDirectory() {}
		virtual IVectorEngine* EngineFor(const void* host) = 0;
	};

	struct EstimateContext
	{
		uint64_t pairSeq = 0;
		bool ahead = false;            // EstimateAhead (the lane) rather than the in-render Estimate
		double nextPairSeconds = 0;    // duration of the pair about to be searched; <= 0 unknown
	};

	// The guidance sink the engine hands to the hook: maps 1:1 to MotionWarp::SetCameraGuidance (null = unbound).
	class IGuidanceSink
	{
	public:
		virtual ~IGuidanceSink() {}
		virtual void SetCameraGuidance(std::shared_ptr<const CameraModel::GuidanceFrame> frame) = 0;
	};

	class IPreEstimateHook
	{
	public:
		virtual ~IPreEstimateHook() {}
		virtual void BeforeEstimate(const EstimateContext& ctx, IGuidanceSink& sink) = 0;
		// countsTex: the 1x1 CameraGuidanceCounts() texture (16 B), null when no guidance ran.
		virtual void AfterEstimate(uint64_t pairSeq, bool guidanceUsed, pl_tex countsTex) = 0;
	};
}
