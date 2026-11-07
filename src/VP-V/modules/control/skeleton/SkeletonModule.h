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

		class SkeletonModule : public Hub::ISkeletonModule
		{
		public:
			static constexpr const char* kName = "skeleton";
			static constexpr const char* kLineKey = "skeleton";
			// The reason shown for the products no producer or live fill can make yet.
			static constexpr const char* kDeclinedReason = "render-thread GPU dispatch hook pending (M2 Upload/Entrance phase)";

			// skeleton_history: engaged only when a capped record history (quality pattern, td trail summary) is in THIS graph.
			static FallbackCatalog::Decision HistoryCapDecision(const VpSkeleton::Graph& graph, double periodMs)
			{
				return FallbackCatalog::SkeletonHistoryCapped(VpSkeleton::GraphHistoryCapNote(graph, periodMs));
			}

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
			// Hub-contained exceptions in this module (all renderers).
			static unsigned long long ContainedFaults();

		private:
			void Reconcile(const void* host, int originalFlag);
			void Teardown(const std::shared_ptr<HostState>& st, bool releaseSlot);
			void Begin(HostState& st, int64_t seq);
			void AfterRender(HostState& st, int64_t seq);
			void Refresh(HostState& st);

			Skeleton::ConfigLoader loader_;
			std::function<uint64_t()> nowUs_;
			Hub::HostSlots<HostState> hosts_;
			bool cardOverride_ = false;
			bool startWorkers_ = true;
			VpSkeleton::CardProfile card_;
		};

		// Adds SkeletonModule to the hub. Start-up only; idempotent. After RegisterResyncExtension.
		void RegisterSkeletonModule();
	}
}
