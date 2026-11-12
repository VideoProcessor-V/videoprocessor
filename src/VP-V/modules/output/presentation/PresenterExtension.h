// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Phase 3 of the host seams plan (refs/107 S4 / S5, refs/108): the presentation module on the hub.
// The renderer makes ONE call per seam and nothing else:
//   R11 PlanPresent   (HA-R11-PlanPresent)   at the entry of its render: the main present's SyncInterval
//                                             (the holds the last plan left after its last extra), and the
//                                             Presenter's frame starts (committed = false);
//   R12 AfterPresent  (HA-R12-AfterPresent)  at the end of its present block (the B3-Z call): the Presenter's
//                                             after-present step (PresenterAfterPresent.h: break the chain or
//                                             raise the host's presentation target lead, publish the open-loop
//                                             estimate), and the present-end fact for R14;
//   R14 ResolveTime   (HA-R14-ResolveTime)   in its render loop: the frame's present end = the main present's
//                                             end when the extras spent time after it, else the host's clock.
// The Presenter itself still lives in the renderer (RenderAhead::Presenter member) and travels in the R11 / R12
// payloads (S8 transitional anchor; Phase 4 keys it by host). Original pathway: the host does not call the
// seams, the Presenter's own fact (SetOriginalPathway) gates the bodies, and the present-end fact is never
// recorded, so R14 returns the host's value.
//
// Threading: R11 / R12 / R14 run on the render thread of one renderer; the present-end fact is per host
// (HostSlots, lock-free), created at the first R12 and dropped at GpuReleased / Shutdown.

#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/core/hub/HostSlots.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace Vpv
{
	namespace Presentation
	{
		class PresenterExtension : public Hub::ISkeletonModule
		{
		public:
			// The present-end fact R12 records and R14 serves (per renderer).
			struct PresentEnd
			{
				const void* host = nullptr;
				std::atomic<int64_t> qpc{ 0 };            // the main present's end (swapEndQpc of the last R12)
				std::atomic<bool> extrasSpentTime{ false };   // that frame's extras presented and spent time after it
				std::atomic<bool> valid{ false };         // set by R12, cleared by R11 (a new frame) and at release
			};

			const char* Name() const override { return "presenter"; }
			bool OnPlanPresent(RendererExtension::FrameContext&, RendererExtension::PresentPlan&) override;
			void OnAfterPresent(RendererExtension::FrameContext&, RendererExtension::PresentContext&) override;
			void OnResolveTime(RendererExtension::FrameContext&, RendererExtension::TimeQuery&) override;
			void OnLifecycle(RendererExtension::LifecycleFacts&) override;

			// Tests: the recorded present end of a host (null = none).
			std::shared_ptr<PresentEnd> PresentEndOf(const void* host) const { return ends_.Find(host); }

		private:
			std::shared_ptr<PresentEnd> EndFor(const void* host, bool create);
			Hub::HostSlots<PresentEnd> ends_;
		};

		void RegisterPresenterExtension();
	}
}
