// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "ExtensionHub.h"

#include <utility>
#include <vector>

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
			{
				std::lock_guard<std::mutex> lock(hostsMutex_);
				hosts_.clear();
			}
			std::vector<const void*> keys;
			pathway_.ForEach([&](const void* key, const std::shared_ptr<PathwayFact>&) { keys.push_back(key); });
			for (const void* key : keys)
				pathway_.Remove(key, true);
		}

		bool ExtensionHub::OriginalPathway(const void* host) const
		{
			const std::shared_ptr<PathwayFact> fact = pathway_.Find(host);
			return fact && fact->original.load(std::memory_order_acquire);
		}

		bool ExtensionHub::NoteOriginalPathway(const void* host, bool original)
		{
			if (!host)
				return false;
			if (const std::shared_ptr<PathwayFact> fact = pathway_.Find(host))
			{
				fact->original.store(original, std::memory_order_release);
				return true;
			}
			std::shared_ptr<PathwayFact> fact = std::make_shared<PathwayFact>();
			fact->host = host;
			fact->original.store(original, std::memory_order_release);
			// False only when the 16-host slot table is full (hosts leave it at GpuReleased / Shutdown, so one
			// renderer never fills it). The hub has no log sink of its own (DebugLog.h brings Windows.h min/max
			// into this translation unit); the caller reads the result and the gate tests pin it to true.
			return pathway_.Put(host, fact);
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
			// PHASE 1: the pathway fact, after the fan-out (a module's SettingsApplied handler compares the old fact
			// with the new f.code).
			if (f.kind == RE::Lifecycle::SettingsApplied && f.host)
				NoteOriginalPathway(f.host, f.code != 0);
			// Released only for this host: a replacement renderer's state is untouched.
			if ((f.kind == RE::Lifecycle::GpuReleased || f.kind == RE::Lifecycle::Shutdown) && f.host)
			{
				{
					std::lock_guard<std::mutex> lock(hostsMutex_);
					hosts_.erase(f.host);
				}
				pathway_.Remove(f.host, true);
			}
		}

		void ExtensionHub::OnSettingsLoad(RE::SettingsLoadContext& c) { for (auto& m : modules_) { try { m->OnSettingsLoad(c); } catch (...) { Contain(c, m->Name()); } } }
		void ExtensionHub::OnIngress(RE::IngressContext& c) { for (auto& m : modules_) { try { m->OnIngress(c); } catch (...) { Contain(c, m->Name()); } } }
		void ExtensionHub::OnQueue(const RE::QueueFacts& f) { for (auto& m : modules_) { try { m->OnQueue(f); } catch (...) { Contain(f, m->Name()); } } }

		// R12: the module sees an empty outcome and writes only its own contribution; it is merged into what the earlier
		// modules (and the caller) left (OR / max / sum). A throw discards the module's partial writes.
		static void AfterPresentMerged(ISkeletonModule& m, RE::FrameContext& c, RE::PresentContext& p)
		{
			const RE::PresentOutcome before = p.outcome;
			p.outcome = RE::PresentOutcome();
			try { m.OnAfterPresent(c, p); }
			catch (...) { p.outcome = before; throw; }
			const RE::PresentOutcome own = p.outcome;
			p.outcome = before;
			p.outcome.extrasRan = before.extrasRan || own.extrasRan;
			p.outcome.committed = before.committed || own.committed;
			if (own.targetLeadMs > before.targetLeadMs) p.outcome.targetLeadMs = own.targetLeadMs;
			if (own.estimatedLeadMs > before.estimatedLeadMs) p.outcome.estimatedLeadMs = own.estimatedLeadMs;
			p.outcome.spentSeconds = before.spentSeconds + own.spentSeconds;
		}

		// One module's share of a phase: the M2 P0b phases go to their typed method (null payload = skipped), the
		// earlier phases to OnFrame as before. Fan-out phases return false (the hub ORs nothing for them).
		static bool DispatchPhase(ISkeletonModule& m, RE::FrameContext& c)
		{
			switch (c.phase)
			{
			case RE::FramePhase::PlanOutputs:   if (auto* p = RE::PayloadOf<RE::OutputPlan>(c)) return m.OnPlanOutputs(c, *p); return false;
			case RE::FramePhase::PlanPresent:   if (auto* p = RE::PayloadOf<RE::PresentPlan>(c)) return m.OnPlanPresent(c, *p); return false;
			case RE::FramePhase::AfterPresent:  if (auto* p = RE::PayloadOf<RE::PresentContext>(c)) AfterPresentMerged(m, c, *p); return false;
			case RE::FramePhase::PresentSample: if (auto* p = RE::PayloadOf<RE::PresentSampleView>(c)) m.OnPresentSample(c, *p); return false;
			case RE::FramePhase::ResolveTime:   if (auto* p = RE::PayloadOf<RE::TimeQuery>(c)) m.OnResolveTime(c, *p); return false;
			case RE::FramePhase::DecideIngress: if (auto* p = RE::PayloadOf<RE::IngressContext>(c)) return m.OnDecideIngress(c, *p); return false;
			case RE::FramePhase::Convert:       if (auto* p = RE::PayloadOf<RE::ConvertContext>(c)) return m.OnConvertedFrame(c, *p); return false;
			case RE::FramePhase::AfterPublish:  if (auto* p = RE::PayloadOf<RE::PublishView>(c)) m.OnAfterPublish(c, *p); return false;
			case RE::FramePhase::Upload:        if (auto* p = RE::PayloadOf<RE::UploadContext>(c)) return m.OnTryUpload(c, *p); return false;
			case RE::FramePhase::QueueTarget:   if (auto* p = RE::PayloadOf<RE::QueueTargetContext>(c)) m.OnQueueTarget(c, *p); return false;
			default:                            return m.OnFrame(c);
			}
		}

		// Take-over phases stop at the first module that returns true; fan-out phases (RE::PhaseFansOut) call every module.
		bool ExtensionHub::OnFrame(RE::FrameContext& c)
		{
			const bool fanOut = RE::PhaseFansOut(c.phase);
			bool handled = false;
			for (auto& m : modules_)
				if (fanOut || !handled) { try { if (DispatchPhase(*m, c)) handled = true; } catch (...) { Contain(c, m->Name()); } }
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
