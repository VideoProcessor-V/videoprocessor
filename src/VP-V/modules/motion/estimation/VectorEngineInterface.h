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
	constexpr const char* kVectorEngineQueryId = "vpv.vector_engine.v1";

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

	class IVectorEngine
	{
	public:
		virtual ~IVectorEngine() {}
		// The newest verified field's seq (0 = none). Monotonic within a stamp era: the "new field" signal.
		virtual uint64_t LatestVerifiedFieldSeq() const = 0;
		virtual bool FieldOf(uint64_t seq, VerifiedField& out) const = 0;
		virtual float FieldScale() const = 0;
		virtual bool GlobalFromEngine() const = 0;
		virtual bool EngineGlobal(uint64_t seq, GlobalVec& out) const = 0;
		virtual bool PairSpansSlip(uint64_t from, uint64_t to) const = 0;
		virtual bool FlowIsCpuResident() const = 0;
		virtual bool FlowOf(uint64_t seq, FlowView& out) const = 0;
		// The camera link registers its hook here (null = none). One hook per engine.
		virtual void SetPreEstimateHook(IPreEstimateHook* hook) = 0;
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
