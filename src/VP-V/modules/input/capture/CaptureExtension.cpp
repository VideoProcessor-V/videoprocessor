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
			std::shared_ptr<HostEntry> entry = std::make_shared<HostEntry>();
			entry->host = host;
			entry->view = std::move(view);
			registered_ = Slots().Put(host, std::move(entry));
			if (!registered_)
				// Visible, never silent: without the binding the capture routes are not served for this renderer
				// (the host's own formatter and upload run instead).
				DebugLog::Log("Capture extension: renderer %p not registered (slot table full or key in use); the capture "
					"routes (capture_path gpu, native_rgb_ingest, dv_tunnel_gpu_unpack, early upload) are off for it", host);
		}

		HostBinding::~HostBinding()
		{
			if (registered_)
				Slots().Remove(host_, true);   // the entry (and its adapter) dies when the last reader lets go
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
