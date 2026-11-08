// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K4): the adapter between the camera link core and P5a's vector engine. It is everything the camera side does about
// guided matching, behind P5a's contract (HOOKS.md section 7): the engine of this renderer comes from Query("vpv.vector_engine.v1")
// -> IVectorEngineDirectory::EngineFor(host); the adapter registers ONE pre-estimate hook on it (VectorEngine::Estimate calls
// BeforeEstimate / AfterEstimate around the matcher's Estimate, in-render and ahead, never for the warm-up pair), and the host
// has no K4 code of its own.
//   * all switches off: nothing is registered, no observer, no state beyond the object itself (the engine runs its Estimate only)
//   * switches on: hook + fit-done observer (the guide lane job) + context provider + slip re-check + counts readback + bench
//   * Detach() clears the engine's hook FIRST (render thread / under renderMutex), then the observer (waiting for a call in flight),
//     then drops everything. Call it at the host's GpuReleased / Shutdown, before the engine pointer goes.
//   * OnFrame() once per frame on the render thread: finish the counts a frame late, pump the lane job.
//   * the d lag is derived at a graph rebuild from the MEASURED costs (RebuildGraph), then the core's meter restarts with the graph.
//   * status through FallbackCatalog decisions and status lines; costs as PassTimers rows (the core posts camera.fit /
//     camera.readback to PassTimers::Inbox::Motion(); no adapter call).
// AfterEstimate: P5a's AfterEstimate(pairSeq, VpMotion::EstimateOutcome) (M2 P5a-out) is mapped by CameraGuidanceHook::AfterEstimate onto
// CameraLink::EstimateOutcome (the ONE mapping: narrow idle, rejection text, gpuMs, counts) and forwarded to AfterEstimateEx.
// C++14, header + .cpp, render thread unless noted.
#pragma once

#include "CameraLinkCore.h"
#include "GuideLaneJob.h"
#include "GuidanceContext.h"
#include "GuidanceScheduler.h"

#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace CameraLink
{
	struct GuidanceAdapterHooks
	{
		P4Reader p4;                                               // P4(N) scene facts from a PUBLISHED slot (required)
		std::function<bool(uint64_t afterSeq, uint64_t& next)> nextPair;   // lane: the first P4-published seq after afterSeq (optional)
		std::function<bool()> onRenderThread;                      // true on the render thread (the engine is render-thread only, P5a contract)
	};

	class CameraGuidanceAdapter
	{
	public:
		CameraGuidanceAdapter(CameraLinkCore& core, const GuidanceSettings& settings, const GuidanceAdapterHooks& hooks);
		~CameraGuidanceAdapter();
		CameraGuidanceAdapter(const CameraGuidanceAdapter&) = delete;
		CameraGuidanceAdapter& operator=(const CameraGuidanceAdapter&) = delete;

		// Binds to this renderer's engine. `counts` = the 16 B readback (ownership taken; null = the counts are never read and the
		// pairs are judged on the plan counters). Registers the hook only when a switch is on.
		void Attach(VpMotion::IVectorEngine* engine, std::unique_ptr<IGuidanceCountsSource> counts);
		// LOCK RULE: Detach (and SetSettings / Attach, which may unregister) run on the render thread with renderMutex held and wait on the core's
		// observer mutex; the camera worker holds that mutex while it pumps the lane job. So the host's p4 reader, nextPair hint and anything
		// they call must NEVER take renderMutex or wait for the render thread, or Detach deadlocks. (CameraLinkCore::SetFitDoneObserver asserts
		// in debug builds that it is not called from the observer's own thread.)
		// engineValid = false: the engine pointer is not known to be valid any more (the renderer was torn down without a GpuReleased, or the
		// directory no longer returns it): the hook is dropped WITHOUT calling the engine (the engine, if it exists, logs a left-over hook).
		void Detach(bool engineValid = true);
		bool Attached() const { return m_engine != nullptr; }
		bool Hooked() const { return m_hooked; }

		// New switches / thresholds: re-registers or unregisters the hook, drops stored lane results.
		void SetSettings(const GuidanceSettings& settings);
		// A graph rebuild: d from the measured fit cost and readback latency over the measured source period (the floor when
		// unmeasured or deterministic). Call BEFORE core.AttachLane / AttachGraph (which restart the meter). Returns d.
		uint8_t RebuildGraph(double sourcePeriodSeconds, bool deterministic);
		// A discontinuity (seek, resync, field era change): stored plans, wanted pairs and holds are dropped.
		void OnDiscontinuity();
		void OnFrame();

		std::vector<FallbackCatalog::Decision> CollectDecisions() const { return m_sched.CollectDecisions(); }
		void CollectStatus(std::vector<std::string>& lines) const;

		GuidanceScheduler& Scheduler() { return m_sched; }
		GuideLaneJob& Job() { return m_job; }
		CameraGuidanceHook& Hook() { return m_hook; }

	private:
		void Reconcile();

		CameraLinkCore& m_core;
		GuidanceAdapterHooks m_hooks;
		VpMotion::IVectorEngine* m_engine = nullptr;
		std::unique_ptr<IGuidanceCountsSource> m_counts;   // before the hook: the hook points at it
		GuidanceSettings m_settings;
		std::atomic<bool> m_flowMayBeResident{ true };   // render thread snapshot of IVectorEngine::FlowIsCpuResident (true until known)
		GuidanceScheduler m_sched;
		CameraGuidanceHook m_hook;
		GuideLaneJob m_job;
		MotionBenchWriter m_bench;
		bool m_hooked = false;
	};
}
