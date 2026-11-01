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
