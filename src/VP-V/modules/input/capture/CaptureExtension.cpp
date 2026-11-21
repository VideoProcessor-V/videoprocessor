// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>
#include "CaptureExtension.h"
#include "CaptureHost.h"

#include <DebugLog.h>

#include <memory>

namespace Vpv
{
	namespace Capture
	{
		namespace
		{
			Hub::HostSlots<HostEntry>& Slots()
			{
				static Hub::HostSlots<HostEntry> slots;
				return slots;
			}
		}

		HostBinding::HostBinding(const void* host, std::unique_ptr<ICaptureHost> view)
			: host_(host)
		{
			if (!host || !view)
				return;
			ICaptureHost* const sink = view.get();   // alive until the entry below is destroyed
			std::shared_ptr<HostEntry> entry = std::make_shared<HostEntry>();
			entry->host = host;
			entry->view = std::move(view);
			registered_ = Slots().Put(host, entry);
			if (registered_)
			{
				// P10 D5, carrier (a): the walk finds this renderer's ring through the core interface (core/SourceRingFeed.h).
				VpSkeleton::SourceRingFeeds::Register(host, std::shared_ptr<VpSkeleton::ISourceRingFeed>(entry, &entry->sourceRing));
				return;
			}
			// Visible, never silent: without the binding the capture routes are not served for this renderer (the host's
			// own formatter and upload run instead): its "Fallbacks engaged" entry, plus the log line.
			sink->SyncFallback(FallbackCatalog::CaptureUnbound(true));
			DebugLog::Log("Capture extension: renderer %p not registered (slot table full or key in use); the capture "
					"routes (capture_path gpu, native_rgb_ingest, dv_tunnel_gpu_unpack, early upload) are off for it", host);
		}

		HostBinding::~HostBinding()
		{
			if (registered_)
			{
				// Teardown law: a walk may still hold the feed's shared_ptr (the last owner of the entry) under the queue lock, so the
				// ring is emptied HERE, on the teardown thread, first: only a Wanted() ring holds hand-offs (Feed is ignored once
				// unwanted), so nothing is left to destroy under that lock when the entry finally dies.
				if (const std::shared_ptr<HostEntry> entry = Slots().Find(host_))
				{
					entry->sourceRing.SetWanted(false);
					entry->sourceRing.ReleaseGraveyard();
				}
				VpSkeleton::SourceRingFeeds::Unregister(host_);
				Slots().Remove(host_, true);
			}   // the entry (and its adapter) dies when the last reader lets go
		}

		std::shared_ptr<HostEntry> Find(const void* host)
		{
			return Slots().Find(host);
		}

		size_t BoundHosts()
		{
			return Slots().States();
		}

		void CaptureExtension::OnLifecycle(RE::LifecycleFacts& f)
		{
			if (f.kind != RE::Lifecycle::SettingsApplied && f.kind != RE::Lifecycle::GpuReleased)
				return;
			const std::shared_ptr<HostEntry> entry = Find(f.host);
			if (!entry || !entry->view)
				return;
			if (f.kind == RE::Lifecycle::SettingsApplied)
				entry->view->ApplySettings();
			else
				entry->view->ReleaseGpu();
		}

		bool CaptureExtension::OnConvertedFrame(RE::FrameContext& c, RE::ConvertContext& cc)
		{
			const std::shared_ptr<HostEntry> entry = Find(c.host);
			return entry && entry->view && entry->view->Convert(cc);
		}

		bool CaptureExtension::OnFrame(RE::FrameContext& c)
		{
			// The graveyard is released here, at Begin, which is a FIRST-WINS phase: an earlier extension returning true would skip this
			// release (the graveyard then waits for the next Begin that reaches it). All current Begin observers return false; if Begin
			// ever gains a consuming handler the release point must move to a fan-out phase (capture README, source ring).
			if (c.phase != RE::FramePhase::Begin)
				return false;
			const std::shared_ptr<HostEntry> entry = Find(c.host);
			if (entry)
				entry->sourceRing.ReleaseGraveyard();   // the only place the ring's references are dropped (no queue lock here)
			return false;
		}

		void CaptureExtension::OnAfterPublish(RE::FrameContext& c, RE::PublishView& pv)
		{
			const std::shared_ptr<HostEntry> entry = Find(c.host);
			if (entry && entry->view)
				entry->view->AfterPublish(pv);
		}

		void RegisterCaptureExtension()
		{
			static bool registered = false;
			if (registered)
				return;
			registered = true;
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(new CaptureExtension()));
		}
	}
}
