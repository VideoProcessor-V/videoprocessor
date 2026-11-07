// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The skeleton module: activates the analysis lane (core/AnalysisLane.h) through the VP-V hub.
// See README.md in this folder for the data sources, what is live and what is declined.
//
// One lane per renderer (keyed by the `host` of every hub callback, never a global "current renderer"):
// the source frames come from the frame-analysis worker through SourceFrameTap, the one seal per frame
// from ResyncExtension's FrameSeal (a provider + a subscriber), the render-thread phases from OnFrame
// (Begin: OnRenderStart; AfterRender: post-submit gap + retire) and OnQueue (Drop). Off (the default):
// no state, no thread, no tap, no status line. The Original pathway forces off.
//
// 70b G5 (GPU products P10/P11/P12): the pl_gpu is stored at GpuCreated; a SkeletonGpuDispatcher (+ the three
// kernels) exists per renderer only while the lane is wanted AND analysis_lane = on AND a pl_gpu was given. Shadow, Off,
// Original and "no render device" never construct one (no GPU call; the status says why). The dispatcher's texture
// allocation (Configure) and its deferred releases run in AfterRender (GPU-legal, before the gap); with
// skeleton_gpu_gap = after_render the GPU lane jobs are registered as lane producers and run in that gap (visible: ahead
// of the present); with off (default) they are not registered (the products stay in-render-only). The live (in-render)
// form is deferred until the Entrance phase (M2 P4): declined and shown.

#include <VP-V/core/AnalysisLane.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/core/hub/HostSlots.h>
#include <VP-V/modules/control/skeleton/SkeletonSettings.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Vpv
{
	namespace Skeleton
	{
		namespace RE = RendererExtension;

		struct HostState;   // per renderer (SkeletonModule.cpp)
		class SkeletonGpuDispatcher;   // gpu/SkeletonGpuDispatcher.h (libplacebo; not included here)
		struct GpuSlotTag { const void* host = nullptr; };   // the per-renderer pl_gpu, kept as a HostSlots aux

		class SkeletonModule : public Hub::ISkeletonModule
		{
		public:
			static constexpr const char* kName = "skeleton";
			static constexpr const char* kLineKey = "skeleton";

			// `loader` reads the configuration (null = the working directory's VideoProcessor.cfg);
			// `nowUs` is the lane's microsecond clock (null = steady_clock).
			explicit SkeletonModule(Skeleton::ConfigLoader loader = Skeleton::ConfigLoader(), std::function<uint64_t()> nowUs = nullptr);
			~SkeletonModule() override;

			const char* Name() const override { return kName; }
			void OnLifecycle(RE::LifecycleFacts&) override;
			void OnQueue(const RE::QueueFacts&) override;
			bool OnFrame(RE::FrameContext&) override;
			void OnStatus(const RE::Context&, RE::StatusKind, std::vector<std::string>&, const char*, const char*) override;
			bool OnCommand(const RE::Context&, const char*, const char*, std::string&) override;

			// Diagnostics and tests.
			bool Active(const void* host) const;                      // the lane is switched on for this renderer
			bool TapRegistered(const void* host) const;
			size_t HostStates() const;
			// The renderer's lane (null while there is none). Render thread / single-threaded tests only.
			VpSkeleton::AnalysisLane* LaneOf(const void* host) const;
			// Dimensions of the current lane graph (0 x 0 without one).
			VpSkeleton::Dims DimsOf(const void* host) const;
			bool WorkersRunning(const void* host) const;
			// Tests: do not start the lane worker threads (the test drives AnalysisLane::StepWorker itself).
			void SetWorkerThreadsForTesting(bool start) { startWorkers_ = start; }
			// Tests: use this card profile instead of the device query (no render device exists in a unit test).
			void SetCardForTesting(const VpSkeleton::CardProfile& card) { cardOverride_ = true; card_ = card; }
			// 70b G5: the renderer's GPU dispatcher (null in Off / Shadow / the Original pathway / without a pl_gpu).
			// Render thread / single-threaded tests only.
			const SkeletonGpuDispatcher* GpuDispatcherFor(const void* host) const;
			// Dispatchers ever constructed by any SkeletonModule (tests: Off and Original must leave it unchanged).
			static unsigned long long GpuDispatchersCreated();
			// Hub-contained exceptions in this module (all renderers).
			static unsigned long long ContainedFaults();

		private:
			void Reconcile(const void* host, int originalFlag);
			void Teardown(const std::shared_ptr<HostState>& st, bool releaseSlot, bool gpuAlive = true);
			void Begin(HostState& st, int64_t seq);
			void AfterRender(HostState& st, int64_t seq);
			void Refresh(HostState& st);

			Skeleton::ConfigLoader loader_;
			std::function<uint64_t()> nowUs_;
			Hub::HostSlots<HostState> hosts_;
			Hub::HostSlots<GpuSlotTag> gpuSlots_;   // aux = the renderer's pl_gpu (GpuCreated), cleared at GpuReleased
			bool cardOverride_ = false;
			bool startWorkers_ = true;
			VpSkeleton::CardProfile card_;
		};

		// Adds SkeletonModule to the hub. Start-up only; idempotent. After RegisterResyncExtension.
		void RegisterSkeletonModule();
	}
}
