// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>
#include "IngressPolicy.h"

#include <VP-V/core/hub/HostSlots.h>
#include <DebugLog.h>

#include <atomic>
#include <memory>

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
				void* self = nullptr;
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

		HostBinding::HostBinding(const void* host, DecideFn decide, void* self)
			: host_(host)
		{
			if (!host || !decide)
				return;
			std::shared_ptr<Entry> entry = std::make_shared<Entry>();
			entry->host = host;
			entry->decide = decide;
			entry->self = self;
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
			return entry && entry->decide && entry->decide(entry->self, ic);
		}

		size_t BoundHosts() { return Slots().States(); }
		size_t UnboundRenderers() { return Unbound().load(std::memory_order_relaxed); }

		void IngressPolicy::OnStatus(const RE::Context&, RE::StatusKind kind, std::vector<std::string>& lines, const char*, const char*)
		{
			if (kind != RE::StatusKind::Fallbacks || UnboundRenderers() == 0)
				return;
			lines.push_back("ingress policy: a renderer could not register; pulldown detection, the film clock and the "
				"stale-delivery gap are off for it");
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
