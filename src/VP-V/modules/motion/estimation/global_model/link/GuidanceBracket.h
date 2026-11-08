// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K4-3): the two-line bracket around MotionWarp::Estimate, so the host's call site (P5a's VectorEngineExtension::
// Estimate / EstimateAhead, or LVR's RunDejudderEstimate before P5a lands) stays generic and inert:
//
//     CameraLink::BeforeGuidedEstimate(preEstimateHook, *dejudder, pairSeq, ahead, pairSeconds);   // before Estimate
//     const bool estimated = dejudder->Estimate(...);
//     CameraLink::AfterGuidedEstimate(preEstimateHook, *dejudder, pairSeq, gpuMs);                 // right after it
//
// With no hook registered (every switch off: nothing is bound) both calls return at once: byte-identical, one branch. The hook
// decides everything else (the refusal gate, lane or live guide); the bracket only
//   * hands the matcher the guide through IGuidanceSink (WarpGuidanceSink maps it 1:1 to MotionWarp::SetCameraGuidance, which
//     the matcher clears itself on every return of Estimate), and
//   * reads back what the matcher did with it (MotionWarp::CameraGuidanceUsed) and where the 16 B counts live
//     (CameraGuidanceCounts, only when a pass that writes them ran: the texture holds an older pair's numbers otherwise).
// Templates over the matcher type, so the CPU tests drive it with a fake and nothing here includes libplacebo or DejudderWarp.h.
// C++14, header-only.
#pragma once

#include "GuidanceScheduler.h"

#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace CameraLink
{
	// IGuidanceSink -> a matcher with SetCameraGuidance(std::shared_ptr<const CameraModel::GuidanceFrame>).
	template <class Warp>
	class WarpGuidanceSink : public VpMotion::IGuidanceSink
	{
	public:
		explicit WarpGuidanceSink(Warp* warp) : m_warp(warp) {}
		void SetCameraGuidance(std::shared_ptr<const CameraModel::GuidanceFrame> frame) override
		{
			if (m_warp)
				m_warp->SetCameraGuidance(std::move(frame));
		}

	private:
		Warp* m_warp;
	};

	// Before Estimate. `pairSeq` is the pair's number in the vector engine's field numbering (the same one IVectorEngine::FieldOf
	// uses); `pairSeconds` the measured duration of the pair about to be searched (<= 0 unknown).
	template <class Warp>
	inline void BeforeGuidedEstimate(VpMotion::IPreEstimateHook* hook, Warp& warp, uint64_t pairSeq, bool ahead, double pairSeconds)
	{
		if (!hook)
			return;   // nothing registered: nothing bound, today's matcher exactly
		WarpGuidanceSink<Warp> sink(&warp);
		VpMotion::EstimateContext ctx;
		ctx.pairSeq = pairSeq;
		ctx.ahead = ahead;
		ctx.nextPairSeconds = pairSeconds;
		hook->BeforeEstimate(ctx, sink);
	}

	// What the matcher reports after Estimate, as the camera link's EstimateOutcome (gpuMs < 0 = not measured).
	template <class Warp>
	inline EstimateOutcome OutcomeOf(const Warp& warp, double gpuMs)
	{
		const auto use = warp.CameraGuidanceUsed();
		EstimateOutcome o;
		o.narrowPass = use.narrowPass;
		o.e1Pass = use.e1Pass;
		o.guidanceUsed = use.modelGlobal || use.staticFromModel || use.matcherSkipped || use.narrowPass || use.e1Pass || use.fadeNormalised;
		o.narrowDroppedEngine = use.narrowDroppedEngine;
		o.rejected = use.rejected;
		o.gpuNotApplied = use.gpuNotApplied;
		o.gpuMs = gpuMs;
		o.counts = (use.narrowPass || use.e1Pass) ? warp.CameraGuidanceCounts() : nullptr;
		return o;
	}

	// After Estimate, through the plain P5a interface (guidanceUsed + the counts texture). The diagnostics that need more than that
	// (narrow idle under the engine, the matcher's rejection text, gpu ms) travel only with AfterGuidedEstimateEx below.
	template <class Warp>
	inline void AfterGuidedEstimate(VpMotion::IPreEstimateHook* hook, Warp& warp, uint64_t pairSeq)
	{
		if (!hook)
			return;
		const EstimateOutcome o = OutcomeOf(warp, -1.0);
		hook->AfterEstimate(pairSeq, o.guidanceUsed, (o.narrowPass || o.e1Pass) ? o.counts : nullptr);
	}

	// After Estimate, with the whole outcome (the camera adapter holds the concrete hook; or P5a's interface carries the outcome).
	template <class Warp>
	inline void AfterGuidedEstimateEx(CameraGuidanceHook* hook, Warp& warp, uint64_t pairSeq, double gpuMs)
	{
		if (!hook)
			return;
		hook->AfterEstimateEx(pairSeq, OutcomeOf(warp, gpuMs));
	}
}
