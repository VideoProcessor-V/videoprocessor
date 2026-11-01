// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "ExtensionHub.h"

#include <utility>

namespace Vpv
{
	namespace Hub
	{
		namespace
		{
			// The registry owns a forwarding wrapper; the real hub lives for the whole process.
			class Forwarder : public RE::IRendererExtension
			{
			public:
				void OnLifecycle(RE::LifecycleFacts& f) override { ExtensionHub::Instance().OnLifecycle(f); }
				void OnSettingsLoad(RE::SettingsLoadContext& c) override { ExtensionHub::Instance().OnSettingsLoad(c); }
				void OnIngress(RE::IngressContext& c) override { ExtensionHub::Instance().OnIngress(c); }
				void OnQueue(const RE::QueueFacts& f) override { ExtensionHub::Instance().OnQueue(f); }
				bool OnFrame(RE::FrameContext& c) override { return ExtensionHub::Instance().OnFrame(c); }
				double OnResolve(RE::Resolve k, const RE::Context& c, double v) override { return ExtensionHub::Instance().OnResolve(k, c, v); }
				void OnStatus(const RE::Context& c, RE::StatusKind k, std::vector<std::string>& l, const char* w, const char* x) override
				{
					ExtensionHub::Instance().OnStatus(c, k, l, w, x);
				}
				bool OnCommand(const RE::Context& c, const char* m, const char* cmd, std::string& r) override
				{
					return ExtensionHub::Instance().OnCommand(c, m, cmd, r);
				}
				void* Query(const char* iid) override { return ExtensionHub::Instance().Query(iid); }
			};
		}

		// A throwing module never stops the modules after it; the fault is reported with its name.
		static void Contain(const RE::Context& ctx, const char* module) noexcept
		{
			try
			{
				std::vector<std::string> none;
				ExtensionHub::Instance().OnStatus(ctx, RE::StatusKind::WorkerFault, none, module ? module : "module", "exception");
			}
			catch (...) {}
		}

		ExtensionHub& ExtensionHub::Instance()
		{
			static ExtensionHub hub;
			return hub;
		}

		void ExtensionHub::AddModule(std::unique_ptr<ISkeletonModule> module)
		{
			if (module) modules_.push_back(std::move(module));
		}

		void ExtensionHub::ResetModulesForTesting()
		{
			modules_.clear();
			std::lock_guard<std::mutex> lock(hostsMutex_);
			hosts_.clear();
		}

		size_t ExtensionHub::LiveHostCount() const
		{
			std::lock_guard<std::mutex> lock(hostsMutex_);
			return hosts_.size();
		}

		bool ExtensionHub::HasHost(const void* host) const
		{
			std::lock_guard<std::mutex> lock(hostsMutex_);
			return hosts_.find(host) != hosts_.end();
		}

		void ExtensionHub::OnLifecycle(RE::LifecycleFacts& f)
		{
			if (f.kind == RE::Lifecycle::GpuCreated && f.host)
			{
				std::lock_guard<std::mutex> lock(hostsMutex_);
				HostState& state = hosts_[f.host];
				state.host = f.host;
				state.hostView = f.hostView;
				state.gpuLive = true;
			}
			for (auto& m : modules_) { try { m->OnLifecycle(f); } catch (...) { Contain(f, m->Name()); } }
			// Released only for this host: a replacement renderer's state is untouched.
			if ((f.kind == RE::Lifecycle::GpuReleased || f.kind == RE::Lifecycle::Shutdown) && f.host)
			{
				std::lock_guard<std::mutex> lock(hostsMutex_);
				hosts_.erase(f.host);
			}
		}

		void ExtensionHub::OnSettingsLoad(RE::SettingsLoadContext& c) { for (auto& m : modules_) { try { m->OnSettingsLoad(c); } catch (...) { Contain(c, m->Name()); } } }
		void ExtensionHub::OnIngress(RE::IngressContext& c) { for (auto& m : modules_) { try { m->OnIngress(c); } catch (...) { Contain(c, m->Name()); } } }
		void ExtensionHub::OnQueue(const RE::QueueFacts& f) { for (auto& m : modules_) { try { m->OnQueue(f); } catch (...) { Contain(f, m->Name()); } } }

		bool ExtensionHub::OnFrame(RE::FrameContext& c)
		{
			bool handled = false;
			for (auto& m : modules_)
				if (!handled) { try { handled = m->OnFrame(c); } catch (...) { Contain(c, m->Name()); } }
			return handled;
		}

		double ExtensionHub::OnResolve(RE::Resolve kind, const RE::Context& ctx, double billValue)
		{
			double value = billValue;
			for (auto& m : modules_) { try { value = m->OnResolve(kind, ctx, value); } catch (...) { Contain(ctx, m->Name()); } }
			return value;
		}

		void ExtensionHub::OnStatus(const RE::Context& ctx, RE::StatusKind kind, std::vector<std::string>& lines, const char* worker, const char* what)
		{
			for (auto& m : modules_) { try { m->OnStatus(ctx, kind, lines, worker, what); } catch (...) { if (kind != RE::StatusKind::WorkerFault) Contain(ctx, m->Name()); } }
		}

		bool ExtensionHub::OnCommand(const RE::Context& ctx, const char* module, const char* command, std::string& reply)
		{
			bool handled = false;
			for (auto& m : modules_)
				if (!handled) { try { handled = m->OnCommand(ctx, module, command, reply); } catch (...) { Contain(ctx, m->Name()); } }
			return handled;
		}

		void* ExtensionHub::Query(const char* iid)
		{
			void* found = nullptr;
			for (auto& m : modules_)
				if (!found) { try { found = m->Query(iid); } catch (...) {} }
			return found;
		}

		void RegisterHub()
		{
			static bool registered = false;
			if (registered) return;
			registered = true;
			ExtensionHub::Instance(); // construct before the registry owns the forwarder, so it is destroyed after it
			RE::Register(std::unique_ptr<RE::IRendererExtension>(new Forwarder()));
		}
	}
}
