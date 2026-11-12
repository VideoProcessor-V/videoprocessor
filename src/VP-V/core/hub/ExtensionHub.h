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
#include <VP-V/core/hub/HostSlots.h>

#include <atomic>
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
			// OnAfterPresent writes only ITS OWN contribution to p.outcome (it starts empty); the hub merges per module
			// (extrasRan/committed OR, targetLeadMs/estimatedLeadMs max, spentSeconds sum).
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

		// PHASE 1 of the host seams plan (refs/107 s2.4 item 1; owner E3 2026-11-11: a VP-V call site in a Bill file does
		// NOTHING in the Original pathway): the pathway the host's settings select, one fact per host, lock-free (HostSlots).
		struct PathwayFact
		{
			const void* host = nullptr;
			std::atomic<bool> original{ false };
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

			// PHASE 1 (refs/107 s2.4 item 1): true while `host` (the renderer's Impl, the key of its SettingsApplied, queue
			// and PlanOutputs calls) runs the Original pathway. Recorded at SettingsApplied (LifecycleFacts::code != 0,
			// CoreSegments.inl UpdatePipelineSlots) AFTER the modules saw that call, so a module's own SettingsApplied
			// handler reads the PREVIOUS pathway here and the new one in f.code (VectorEngineExtension uses the pair);
			// dropped with the host (GpuReleased / Shutdown). No fact (before the first settings apply, an unknown host,
			// a full table: logged once per host) = VP-V, i.e. the module runs as before. Any thread: one slot scan.
			bool OriginalPathway(const void* host) const;
			// The recorder (the hub's SettingsApplied bookkeeping; tests). False = not recorded (null host / table full).
			bool NoteOriginalPathway(const void* host, bool original);

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
			HostSlots<PathwayFact> pathway_;   // PHASE 1: the per-host Original fact (lock-free readers on the frame paths)
		};

		// Registers the hub with the hook layer exactly once (idempotent). Called by the
		// bootstrap translation unit at load; tests may call it too.
		void RegisterHub();
	}
}
