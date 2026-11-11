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

	// F2 (camera fit input): the optical-flow engine's SEED of one source pair (the searching matcher's ofSearch pair, the
	// engine's block vectors that stood in for the matcher's first sweep), which the camera guidance never shapes. A DISTINCT
	// type from VerifiedField on purpose (provenance): there is no conversion between the two, so a consumer that takes a
	// SeedField can never be handed the guided live field (FieldOf). Textures are owned by the engine and valid until the
	// next estimate (the consumer copies, never keeps the pl_tex).
	// Layout (checked, never assumed): gridW x gridH texels, one per blockSize x blockSize SOURCE px (16 x 2^halvings: 16
	// at 1080p, 32 at a lowRes 4K search, 64 at 8K), rgba = (vx, vy) in seed px (x scaleToSourcePx = source px; 2 x
	// 2^halvings), the block SAD (8-bit codes per px; >= 1e4 = beyond the searches' reach), the engine weight (0..1, 1 =
	// none). sourceGridW x sourceGridH is the 16-px source-block grid the live field has (the camera fit's grid); the
	// seed is converted onto it on the CPU (each 16-px block takes the seed texel its centre lies in), trust = the live
	// consistency pass's rule on the seed (forward/backward agreement within consistencyPx seed px AND the SAD, raised by
	// (1 - weight) x weightPenalty, within sadTrustMax; sadTrustMax = taper_sad with block_taper on, else match_tolerance;
	// weightPenalty = 2 x sadTrustMax when the engine's trust weights are applied, else 0).
	enum class SeedAbsence
	{
		NotSeeded,   // engine off or failed, matcher-only, candidate / replace mode, no such live pair, layout mismatch
		Skipped,     // the live pair's searches were skipped (camera guidance: a cut / flash): no seed for that pair only
	};
	struct SeedField
	{
		uint64_t seq = 0, pairFrom = 0, pairTo = 0;
		pl_tex fwd = nullptr;            // A -> B seed (rgba, see above)
		pl_tex bwd = nullptr;            // B -> A seed (the consistency check only; the fit reads no backward field)
		int gridW = 0, gridH = 0;        // seed texels
		int blockSize = 0;               // source px per seed texel
		float scaleToSourcePx = 0.0f;    // seed px -> source px
		int sourceGridW = 0, sourceGridH = 0;
		enum { kSourceBlockSize = 16 };   // source px per block of the fit's grid (the live field's)
		float sadTrustMax = 0.0f;        // the live SAD limit (taper_sad with block_taper, else match_tolerance)
		float weightPenalty = 0.0f;      // 2 x sadTrustMax with the engine's trust weights applied, else 0
		float consistencyPx = 0.0f;      // the searching matcher's forward/backward agreement, seed px
		SeedAbsence absence = SeedAbsence::NotSeeded;   // why, when SeedFieldOf returned false
		VpIndicator::Stamp stamp;        // the stamp the pair was estimated under
	};

	struct GlobalVec
	{
		bool valid = false;
		double x = 0, y = 0;         // source px over the pair
	};

	// W1-C3 (doc 93 D3/D5): the per-cell engine trust of a served pair, memoized per pair by the engine's FlowOf lease (computed once
	// per pair and tolerance, whoever asks). Implemented by the engine; a FlowView without one (a fake, an older engine) answers null.
	// The API for later FlowOf consumers (dither, scaling); the camera ahead fit computes its CellTrust once per pair on its own worker
	// from the copied extras by design (its tolerance lives there), and TD reads the P2 view directly.
	// Lifetime / thread: render thread only (FlowOf's contract), never from the camera worker; a returned pointer stays valid until the
	// next FlowOf call, also when another tolerance is asked in between (one stable plane per tolerance). Tolerances compare by bit value
	// (1.5f matches only 1.5f; -0 and +0 are two entries; a repeated NaN is one entry, never an unbounded list).
	class IFlowTrustMemo
	{
	public:
		virtual ~IFlowTrustMemo() = default;
		// OpticalFlowTrust::CellTrust of the pair's forward direction (gridW x gridH) with the matcher's consistency limit in engine px;
		// null when the pair has no backward vectors or no forward costs (never guessed). Valid until the next FlowOf.
		virtual const float* CellTrust(float toleranceEnginePx) const = 0;
	};

	// A CPU-resident flow view (for temporalSeed); null planes when the flow is GPU-only.
	struct FlowView
	{
		const float* x = nullptr;
		const float* y = nullptr;
		int gridW = 0, gridH = 0;
		double blockSize = 0;
		uint64_t from = 0, to = 0;
		// The producer's ORIGINAL payload at its NATIVE NVOF grid and full precision (no resampled or quantised copy): the same memory the
		// P2 slot holds (kept alive by the engine's lease until the next FlowOf call). Vectors are interleaved (x, y) per cell, row-major
		// gridW x gridH, in signed fixed point: value / rawUnitsPerEnginePx = engine px; x enginePxToSource = source px. A cell covers
		// cellEnginePx x cellEnginePx engine px = blockSize source px. Costs are one byte per cell (higher = less sure). Any pointer may be
		// null (backward / costs empty); rawForward is non-null whenever FlowOf returned true. x / y above are these values converted
		// to source px (float).
		const int16_t* rawForward = nullptr;
		const int16_t* rawBackward = nullptr;
		const uint8_t* forwardCost = nullptr;
		const uint8_t* backwardCost = nullptr;
		double rawUnitsPerEnginePx = 0;   // 32 (S10.5)
		double enginePxToSource = 0;      // source px per engine px (source width / engine width)
		int cellEnginePx = 0;             // 4
		int engineW = 0, engineH = 0;     // the engine-size luma the flow is for
		// W1-C3 (doc 93 D3/D5): the producer's EXTRAS, worked out ONCE on its worker (MotionAhead::FlowGlobals / FlowExtras) and
		// served as they are (the same memory as the P2 slot; lease lifetime as above). Consumers read these instead of recomputing.
		//  * global / second: MotionAhead::FlowGlobal of the forward [0] and backward [1] fields (D1 format: x, y in engine cells,
		//    ambiguity, valid). Meaningful only when haveGlobal; globalGeneral = the general_rate_fixes flag it was voted with.
		//  * costWeight: OpticalFlowTrust::CostWeights of the forward costs per trust block (extrasBlockPx engine px, blocksX x blocksY).
		//  * fbError: OpticalFlowTrust::FbError(forward, backward) per cell (gridW x gridH, engine px).
		//  extrasCellPx / extrasUnitsPerEnginePx: the cell size and fixed-point units the extras were computed for; a consumer whose
		//  arithmetic differs recomputes. Null / 0 when the pair carries none (then the consumer computes them itself, as before).
		bool haveGlobal = false;
		bool globalGeneral = true;
		float global[2][4] = {}, second[2][4] = {};
		const float* costWeight = nullptr;
		int blocksX = 0, blocksY = 0;
		int extrasBlockPx = 0;
		const float* fbError = nullptr;
		int extrasCellPx = 0;
		double extrasUnitsPerEnginePx = 0;
		// The engine's per-cell trust (OpticalFlowTrust::CellTrust), computed once per pair id and tolerance by the lease; null when
		// the view has no memo or the pair has no backward vectors / forward costs.
		const IFlowTrustMemo* trustMemo = nullptr;
		const float* CellTrust(float toleranceEnginePx) const { return trustMemo ? trustMemo->CellTrust(toleranceEnginePx) : nullptr; }
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
		// F2: the optical-flow seed of the pair whose later frame is pairTo, under FieldOf's rules (the last successful
		// estimate only, its epoch current, valid until the next estimate, render thread / renderMutex). False whenever
		// the seed did not feed that pair (engine off or failed, matcher-only, candidate or replace mode, a skipped pair).
		// The camera fit reads ONLY this (never FieldOf: that field is the guided one). On false, out is reset and
		// out.absence says why. Default: absent (existing fakes).
		virtual bool SeedFieldOf(uint64_t /*pairTo*/, SeedField& out) const { out = SeedField(); return false; }
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
		// SK-W stage D: the camera link (through the skeleton's external demand) says the camera fit wants the verified field: the engine
		// counts it as the consumer VectorDemand::Consumer::CameraFit at its next decision (any thread: an atomic). The default ignores it.
		virtual void SetCameraFitWanted(bool /*wanted*/) {}
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

	// What the matcher's estimate did with the guidance the hook bound, as plain data (M2 P5a-out). Built by the engine
	// right after the matcher's Estimate (or, when the estimate threw, with every field absent) and passed by const
	// reference to IPreEstimateHook::AfterEstimate; valid for the duration of that call only. Every field has an
	// "absent" value, so a hook that reads a field the engine cannot fill yet sees "unknown", never a wrong value.
	struct EstimateOutcome
	{
		// MotionWarp::CameraGuidanceUsed() reports a guided part (model global, static-from-model, matcher skipped,
		// narrow pass or E1 pass) and no rejection. Absent: false (also when the estimate itself threw).
		bool guidanceUsed = false;
		// CameraGuidanceUsed().narrowDroppedEngine (MotionWarp's "narrowDroppedEngine"): the narrow pass was planned but
		// the optical-flow engine seeded or replaced the searches, so the narrow pass stayed idle. Absent: false.
		bool narrowIdle = false;
		// CameraGuidanceUsed().rejected: the reason the matcher did not use the per-block guidance (for example "camera
		// guidance: texture upload failed"). A static-lifetime string (the matcher's literals): the hook may keep the
		// pointer, never frees it. Absent: nullptr (the matcher's empty string is reported as nullptr).
		const char* rejection = nullptr;
		// The GPU time of this estimate in ms. The matcher's per-pass GPU timers (PassTimers, sampled 1 frame in N and
		// polled a frame later, TakePassGpuMs) are asynchronous and are collected by the renderer after the estimate
		// returns, so no per-estimate figure exists at this point and the engine adds no GPU timer query for it.
		// Absent / unknown: -1.0 (today always -1.0).
		double gpuMs = -1.0;
		// The 1x1 CameraGuidanceCounts() texture (16 B: r/g = narrow accepted fwd/bwd, b/a = E1 skipped fwd/bwd, -1 = not
		// run). Engine-owned: valid only until the next estimate; the hook copies, never keeps it. Absent: nullptr (no
		// guidance used, or neither a narrow nor an E1 pass ran). Typed pl_tex, as the parameter it replaces was.
		pl_tex countsTex = nullptr;
	};

	class IPreEstimateHook
	{
	public:
		virtual ~IPreEstimateHook() {}
		virtual void BeforeEstimate(const EstimateContext& ctx, IGuidanceSink& sink) = 0;
		// pairSeq: the pair's later frame (EstimateContext::pairSeq). outcome: see EstimateOutcome.
		virtual void AfterEstimate(uint64_t pairSeq, const EstimateOutcome& outcome) = 0;
	};
}
