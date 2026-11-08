// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>
#include "DejudderExtension.h"

#include <VP-V/core/hub/HostSlots.h>
#include <DebugLog.h>

#include <cstring>
#include <memory>

namespace VpMotion
{
	namespace
	{
		struct Entry
		{
			const void* host = nullptr;
			DejudderBindingSpec spec;
		};
		Vpv::Hub::HostSlots<Entry>& Slots()
		{
			static Vpv::Hub::HostSlots<Entry> slots;
			return slots;
		}

		class Directory : public IDejudderDirectory
		{
		public:
			IDejudder* DejudderFor(const void* host) override
			{
				const std::shared_ptr<Entry> e = Slots().Find(host);
				return e ? e->spec.dejudder : nullptr;
			}
			IPhaseTimeline* PhaseTimelineFor(const void* host) override
			{
				const std::shared_ptr<Entry> e = Slots().Find(host);
				return e ? e->spec.phaseTimeline : nullptr;
			}
		};
		Directory& TheDirectory()
		{
			static Directory directory;
			return directory;
		}
	}

	DejudderHostBinding::DejudderHostBinding(const void* host, const DejudderBindingSpec& spec)
		: host_(host)
	{
		if (!host)
			return;
		std::shared_ptr<Entry> entry = std::make_shared<Entry>();
		entry->host = host;
		entry->spec = spec;
		registered_ = Slots().Put(host, entry);
		if (!registered_)
			DebugLog::Log("De-judder module: renderer %p not registered (slot table full or key in use); the per-refresh "
				"timeline does not plan for it", host);
	}

	DejudderHostBinding::~DejudderHostBinding()
	{
		if (registered_)
			Slots().Remove(host_, true);
	}

	void* DejudderExtension::Query(const char* iid)
	{
		if (!iid || (std::strcmp(iid, kDejudderQueryId) != 0 && std::strcmp(iid, kPhaseTimelineQueryId) != 0))
			return nullptr;
		return static_cast<void*>(static_cast<IDejudderDirectory*>(&TheDirectory()));
	}

	// The render loop's backlog flush (the queue lock is held by the caller; no lock here).
	void DejudderExtension::OnQueue(const RE::QueueFacts& f)
	{
		if (f.kind != RE::QueueEvent::Flush)
			return;
		const std::shared_ptr<Entry> e = Slots().Find(f.host);
		if (e && e->spec.queueFlush)
			e->spec.queueFlush(e->spec.self);
	}

	// Render loop, renderMutex held: the registered renderer's timeline plan.
	bool DejudderExtension::OnPlanOutputs(RE::FrameContext& c, RE::OutputPlan& plan)
	{
		const std::shared_ptr<Entry> e = Slots().Find(c.host);
		return e && e->spec.planOutputs ? e->spec.planOutputs(e->spec.self, plan) : false;
	}

	void RegisterDejudderExtension()
	{
		static bool registered = false;
		if (registered)
			return;
		registered = true;
		Vpv::Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new DejudderExtension()));
	}
}
