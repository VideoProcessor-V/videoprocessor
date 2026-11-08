// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The VP-V skeleton dispatcher (M2 P0). It is the ONE RendererExtension::IRendererExtension VP-V
// registers with the generic hook layer (vprenderer/RendererExtension.h). Modules never talk to
// the host or to each other through it: they implement ISkeletonModule, the hub fans each
// callback out to them in a fixed order, and they exchange data through FrameAnalysisStore,
// products and FrameSeal. With no module added the hub is inert (empty loops), so the renderer
// behaves exactly as without it. Original pathway: a module returns immediately when its switch
// is off; the hub itself has no behaviour and no defaults.
//
// Threading: AddModule() runs at start-up only, before any renderer/capture thread; afterwards the
// module list is read-only and dispatch takes no lock. The per-host state map is guarded by a
// mutex taken only on GpuCreated / GpuReleased / Shutdown (never on a per-frame path), because
// RetireOutput can run after a replacement renderer has started: state is keyed by host and
// released only for that host.

#include <vprenderer/RendererExtension.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Vpv
{
	namespace Hub
	{
		namespace RE = RendererExtension;

		// One VP-V module's view of the skeleton. All callbacks default to "nothing".
		class ISkeletonModule
		{
		public:
			virtual ~ISkeletonModule() = default;
			virtual const char* Name() const = 0;
			virtual void OnLifecycle(RE::LifecycleFacts&) {}
			virtual void OnSettingsLoad(RE::SettingsLoadContext&) {}
			virtual void OnIngress(RE::IngressContext&) {}
			virtual void OnQueue(const RE::QueueFacts&) {}
			virtual bool OnFrame(RE::FrameContext&) { return false; }
			// M2 P0b typed phases (R10-R19). For these phases the hub decodes FrameContext::phaseData and calls the
			// method below instead of OnFrame (a phase whose payload is null is skipped). Defaults = "not handled" /
			// "no change". Take-over phases (first-wins, bool): PlanOutputs, PlanPresent, DecideIngress, Convert,
			// Upload. Fan-out phases (every module, in order, void): AfterPresent, PresentSample, ResolveTime,
			// AfterPublish, QueueTarget; ResolveTime and QueueTarget are chained (each sees the previous result).
			// Threads and locks per hook are in RendererExtension.h and doc 59 section 1.1; DecideIngress runs on the
			// capture thread and ResolveQueueTarget under the host's queue mutex: no blocking, no locks.
			virtual bool OnPlanOutputs(RE::FrameContext&, RE::OutputPlan&) { return false; }
			virtual bool OnPlanPresent(RE::FrameContext&, RE::PresentPlan&) { return false; }
			virtual void OnAfterPresent(RE::FrameContext&, RE::PresentContext&) {}
			virtual void OnPresentSample(RE::FrameContext&, RE::PresentSampleView&) {}
			virtual void OnResolveTime(RE::FrameContext&, RE::TimeQuery&) {}
			virtual bool OnDecideIngress(RE::FrameContext&, RE::IngressContext&) { return false; }
			virtual bool OnConvertedFrame(RE::FrameContext&, RE::ConvertContext&) { return false; }
			virtual void OnAfterPublish(RE::FrameContext&, RE::PublishView&) {}
			virtual bool OnTryUpload(RE::FrameContext&, RE::UploadContext&) { return false; }
			virtual void OnQueueTarget(RE::FrameContext&, RE::QueueTargetContext&) {}
			virtual double OnResolve(RE::Resolve, const RE::Context&, double billValue) { return billValue; }
			virtual void OnStatus(const RE::Context&, RE::StatusKind, std::vector<std::string>&, const char*, const char*) {}
			virtual bool OnCommand(const RE::Context&, const char*, const char*, std::string&) { return false; }
			virtual void* Query(const char*) { return nullptr; }
		};

		struct HostState
		{
			const void* host = nullptr;
			void* hostView = nullptr;
			bool gpuLive = false;
		};

		class ExtensionHub : public RE::IRendererExtension
		{
		public:
			static ExtensionHub& Instance();

			// Start-up only. Fixed order = fan-out order.
			void AddModule(std::unique_ptr<ISkeletonModule> module);
			size_t ModuleCount() const { return modules_.size(); }
			void ResetModulesForTesting();
			// Host bookkeeping (diagnostics and tests).
			size_t LiveHostCount() const;
			bool HasHost(const void* host) const;

			void OnLifecycle(RE::LifecycleFacts&) override;
			void OnSettingsLoad(RE::SettingsLoadContext&) override;
			void OnIngress(RE::IngressContext&) override;
			void OnQueue(const RE::QueueFacts&) override;
			bool OnFrame(RE::FrameContext&) override;
			double OnResolve(RE::Resolve, const RE::Context&, double billValue) override;
			void OnStatus(const RE::Context&, RE::StatusKind, std::vector<std::string>&, const char*, const char*) override;
			bool OnCommand(const RE::Context&, const char*, const char*, std::string&) override;
			void* Query(const char*) override;

		private:
			std::vector<std::unique_ptr<ISkeletonModule>> modules_;
			mutable std::mutex hostsMutex_;
			std::map<const void*, HostState> hosts_;
		};

		// Registers the hub with the hook layer exactly once (idempotent). Called by the
		// bootstrap translation unit at load; tests may call it too.
		void RegisterHub();
	}
}
