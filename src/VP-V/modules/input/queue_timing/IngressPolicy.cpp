// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>
#include "IngressPolicy.h"
#include "IngressState.h"

#include <VP-V/core/RunningPathway.h>
#include <VP-V/core/hub/HostSlots.h>
#include <DebugLog.h>

#include <atomic>
#include <memory>
#include <string>

namespace Vpv
{
	namespace Ingress
	{
		namespace
		{
			// Single-word pathway state: bit 0 = active Original pathway flag, upper 63 bits = monotonic update
			// sequence so a BindImplHost sync can never overwrite a concurrent SettingsApplied / NoteOriginalPathway.
			constexpr uint64_t kOriginalBit = 1u;
			constexpr uint64_t kSeqStep = 2u;

			struct Entry
			{
				const void* host = nullptr;
				DecideFn decide = nullptr;
				QueueTargetFn queueTarget = nullptr;
				FaultFn fault = nullptr;
				void* self = nullptr;
				std::atomic<uint64_t> faults{ 0 };   // this renderer's contained throws (slice 4 F4: per renderer)
				std::atomic<const void*> implHost{ nullptr };   // S1-M1: associated Impl* for SettingsApplied / GpuReleased
				std::atomic<uint64_t> pathwayState{ 0 };        // PHASE 1 + S1-M1: (seq << 1) | originalBit
			};

			void StorePathway(Entry& entry, bool original)
			{
				uint64_t cur = entry.pathwayState.load(std::memory_order_acquire);
				for (;;)
				{
					const uint64_t next = ((cur & ~kOriginalBit) + kSeqStep) | (original ? kOriginalBit : 0u);
					if (entry.pathwayState.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire))
						return;
				}
			}

			Hub::HostSlots<Entry>& Slots()
			{
				static Hub::HostSlots<Entry> slots;
				return slots;
			}
			std::atomic<size_t>& Unbound()
			{
				static std::atomic<size_t> count{ 0 };
				return count;
			}
		}

		HostBinding::HostBinding(const void* host, DecideFn decide, void* self, QueueTargetFn queueTarget, FaultFn fault)
			: host_(host)
		{
			if (!host || !decide)
				return;
			std::shared_ptr<Entry> entry = std::make_shared<Entry>();
			entry->host = host;
			entry->decide = decide;
			entry->self = self;
			entry->queueTarget = queueTarget;
			entry->fault = fault;
			entry->pathwayState.store(Vpv::RunningPathway::Original() ? kOriginalBit : 0u, std::memory_order_release);
			registered_ = Slots().Put(host, entry);
			if (registered_)
				return;
			unbound_ = true;
			Unbound().fetch_add(1, std::memory_order_relaxed);
			DebugLog::Log("Ingress policy: renderer %p not registered (slot table full or key in use); pulldown detection, "
				"the film clock and the stale-delivery gap are off for it", host);
		}

		HostBinding::~HostBinding()
		{
			if (registered_)
				Slots().Remove(host_, true);
			if (unbound_)
				Unbound().fetch_sub(1, std::memory_order_relaxed);
		}

		bool Decide(RE::IngressContext& ic)
		{
			const std::shared_ptr<Entry> entry = Slots().Find(ic.host);
			if (!entry || !entry->decide)
				return false;
			const int64_t arrivalBefore = ic.arrivalQpc;
			// Slice 4 F2: the host's own gap facts, restored if the decision throws half-way.
			const bool gapBefore = ic.gapBefore, discontinuity = ic.discontinuity;
			const uint32_t gapFrames = ic.gapFrames;
			const char* const gapCause = ic.gapCause;
			try
			{
				return entry->decide(entry->self, ic);
			}
			catch (...)
			{
			}
			// Audit slice 3 F1: visible, never silent. The frame is queued (no drop), as a no-fill gap without film
			// timing at its pre-decision arrival; the detector starts over on the next frame.
			const uint64_t faults = entry->faults.fetch_add(1, std::memory_order_relaxed) + 1;
			if ((faults & (faults - 1)) == 0)
				DebugLog::Log("Ingress policy: the pulldown / film clock decision threw (%llu so far); the frame is queued as a gap "
					"and the detector restarts", static_cast<unsigned long long>(faults));
			ic.drop = false;
			ic.gapBefore = gapBefore;
			ic.gapFrames = gapFrames;
			ic.gapCause = gapCause;
			ic.discontinuity = discontinuity;
			if (!ic.gapBefore)
			{
				ic.gapBefore = true;   // never a pair across the fault
				ic.gapFrames = 0;
				ic.gapCause = "ingress policy fault";
			}
			ic.arrivalQpc = arrivalBefore;
			ic.filmHz = 0.0;
			ic.filmUpdate = false;
			if (entry->fault)
			{
				try { entry->fault(entry->self); }
				catch (...) {}
			}
			return true;
		}

		void QueueTarget(const void* host, RE::QueueTargetContext& qc)
		{
			const std::shared_ptr<Entry> entry = Slots().Find(host);
			if (entry && entry->queueTarget)
				entry->queueTarget(entry->self, qc);
		}

		void BindImplHost(const void* rendererHost, const void* implHost)
		{
			if (!rendererHost)
				return;
			const std::shared_ptr<Entry> entry = Slots().Find(rendererHost);
			if (!entry)
				return;
			uint64_t stateBefore = entry->pathwayState.load(std::memory_order_acquire);
			if (entry->implHost.exchange(implHost, std::memory_order_acq_rel) == implHost)
				return;
			if (!implHost)
				return;
			std::atomic_thread_fence(std::memory_order_seq_cst);
			uint32_t fixBits = 0;
			bool synced = false;
			bool original = false;
			if (Hub::ExtensionHub::Instance().FixSwitchBits(implHost, fixBits))
			{
				synced = true;
				original = IngressState::FixSwitchesFromBits(fixBits).originalPathway;
			}
			else if (Hub::ExtensionHub::Instance().OriginalPathway(implHost))
			{
				synced = true;
				original = true;
			}
			if (synced)
			{
				const uint64_t next = ((stateBefore & ~kOriginalBit) + kSeqStep) | (original ? kOriginalBit : 0u);
				entry->pathwayState.compare_exchange_strong(stateBefore, next, std::memory_order_acq_rel, std::memory_order_relaxed);
			}
		}

		void NoteOriginalPathway(const void* host, bool original)
		{
			if (const std::shared_ptr<Entry> entry = Slots().Find(host))
				StorePathway(*entry, original);
		}

		bool OriginalPathwayOf(const void* host)
		{
			const std::shared_ptr<Entry> entry = Slots().Find(host);
			return entry && (entry->pathwayState.load(std::memory_order_acquire) & kOriginalBit) != 0;
		}

		void SyncHostFromImpl(const void* rendererHost, const void* implHost, IngressState& state,
			std::atomic<bool>& queueTimingFixes, std::atomic<bool>& motionHistoryFixes)
		{
			if (!rendererHost || !implHost)
				return;
			BindImplHost(rendererHost, implHost);
			const std::shared_ptr<Entry> entry = Slots().Find(rendererHost);
			uint64_t stateBefore = entry ? entry->pathwayState.load(std::memory_order_acquire) : 0u;
			uint32_t fixBits = 0;
			if (Hub::ExtensionHub::Instance().FixSwitchBits(implHost, fixBits))
			{
				const IngressState::FixSwitches switches = IngressState::FixSwitchesFromBits(fixBits);
				state.SyncIngressSwitches(switches, queueTimingFixes, motionHistoryFixes);
				if (entry)
				{
					const uint64_t next = ((stateBefore & ~kOriginalBit) + kSeqStep) | (switches.originalPathway ? kOriginalBit : 0u);
					entry->pathwayState.compare_exchange_strong(stateBefore, next, std::memory_order_acq_rel, std::memory_order_acquire);
				}
			}
		}

		size_t BoundHosts() { return Slots().States(); }
		size_t UnboundRenderers() { return Unbound().load(std::memory_order_relaxed); }
		uint64_t IngressFaults()
		{
			uint64_t total = 0;   // over the live renderers only: a retired renderer's faults go with it
			Slots().ForEach([&](const void*, const std::shared_ptr<Entry>& e) { total += e->faults.load(std::memory_order_relaxed); });
			return total;
		}

		void IngressPolicy::OnLifecycle(RE::LifecycleFacts& f)
		{
			if (!f.host)
				return;
			if (f.kind == RE::Lifecycle::SettingsApplied)
			{
				std::atomic_thread_fence(std::memory_order_seq_cst);
				const bool original = f.code != 0;
				Slots().ForEach([&](const void*, const std::shared_ptr<Entry>& entry)
				{
					if (entry->implHost.load(std::memory_order_acquire) == f.host)
						StorePathway(*entry, original);
				});
			}
			else if (f.kind == RE::Lifecycle::GpuReleased || f.kind == RE::Lifecycle::Shutdown)
			{
				Slots().ForEach([&](const void*, const std::shared_ptr<Entry>& entry)
				{
					const void* expected = f.host;
					entry->implHost.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel, std::memory_order_relaxed);
				});
			}
		}

		// Original pathway: not handled, so Bill's pre-filled facts stand (his OnVideoFrame: no pulldown, no stale gap).
		bool IngressPolicy::OnDecideIngress(RE::FrameContext&, RE::IngressContext& ic)   // VPV-GATED
		{
			const bool originalPathway = OriginalPathwayOf(ic.host);
			if (originalPathway) return false;
			return Decide(ic);
		}

		// Original pathway: qc.target stays Bill's own value (PrefillTargetLocked's ClampDesiredDepthToCapacity, which
		// BillQueuePolicy::QueueTarget computed from the same inputs before).
		void IngressPolicy::OnQueueTarget(RE::FrameContext& c, RE::QueueTargetContext& qc)   // VPV-GATED
		{
			const bool originalPathway = OriginalPathwayOf(c.host);
			if (originalPathway) return;
			QueueTarget(c.host, qc);
		}

		void IngressPolicy::OnStatus(const RE::Context&, RE::StatusKind kind, std::vector<std::string>& lines, const char*, const char*)
		{
			if (kind != RE::StatusKind::Fallbacks)
				return;
			// One id per line (FallbackStatus::Ids splits a heading on top-level "; ").
			if (UnboundRenderers() > 0)
				lines.push_back("ingress policy (renderer not registered: pulldown, film clock, stale gap off)");
			if (const uint64_t faults = IngressFaults())
				lines.push_back("ingress_fault (" + std::to_string(faults) + " pulldown/film clock exception(s); detector restarted)");
		}

		void RegisterIngressPolicy()
		{
			static bool registered = false;
			if (registered)
				return;
			registered = true;
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(new IngressPolicy()));
		}
	}
}
