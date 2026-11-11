// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>
#include "IngressPolicy.h"

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
			struct Entry
			{
				const void* host = nullptr;
				DecideFn decide = nullptr;
				QueueTargetFn queueTarget = nullptr;
				FaultFn fault = nullptr;
				void* self = nullptr;
				std::atomic<uint64_t> faults{ 0 };   // this renderer's contained throws (slice 4 F4: per renderer)
				std::atomic<bool> originalPathway{ false };   // PHASE 1: the renderer's active pathway (NoteOriginalPathway)
			};
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

		void NoteOriginalPathway(const void* host, bool original)
		{
			if (const std::shared_ptr<Entry> entry = Slots().Find(host))
				entry->originalPathway.store(original, std::memory_order_release);
		}

		bool OriginalPathwayOf(const void* host)
		{
			const std::shared_ptr<Entry> entry = Slots().Find(host);
			return entry && entry->originalPathway.load(std::memory_order_acquire);
		}

		size_t BoundHosts() { return Slots().States(); }
		size_t UnboundRenderers() { return Unbound().load(std::memory_order_relaxed); }
		uint64_t IngressFaults()
		{
			uint64_t total = 0;   // over the live renderers only: a retired renderer's faults go with it
			Slots().ForEach([&](const void*, const std::shared_ptr<Entry>& e) { total += e->faults.load(std::memory_order_relaxed); });
			return total;
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
