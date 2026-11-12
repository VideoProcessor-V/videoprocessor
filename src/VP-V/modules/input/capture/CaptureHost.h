// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P10 (doc 59 section 5.2): the renderer's side of the capture extension.
//
// The capture routes, the early upload and the unpackers need the renderer's own state (its GPU, formatter,
// queue target, fallbacks, timers). As in P5a (VectorEngineHost.h), the moved code stays the renderer's Impl
// member functions, defined in VP-V (CaptureImpl.inl, included by the renderer after its Impl): the text moves
// verbatim and keeps naming the members it always named. This header gives the hub module a way to reach them:
//
//   ICaptureHost       what the module calls (one method per hook it serves)
//   CaptureImplHost<I> the header-only adapter over the renderer's Impl (forwards by name, holds a reference)
//   HostBinding        ONE Impl member, declared after the capture-path members (destroyed before them; since P11a-0 the
//                      S8 anchor `Vpv::HostState vpv;` follows it as the Impl's LAST member, in the same tagged run):
//                        Vpv::Capture::HostBinding captureBinding{ this, Vpv::Capture::MakeCaptureHost(*this) };
//                      registers the adapter under `host` (the Impl, the key every hook passes) for the member's
//                      lifetime, in a fixed lock-free slot table.
//
// Threading: Find() is lock-free (a key compare and one shared_ptr load) and may run on the capture, conversion,
// render, file or settings thread. The binding is built with the Impl and removed before any other Impl member is
// destroyed; the renderer's threads that raise these hooks (conversion worker, render thread) are joined before the
// Impl is destroyed, so a found adapter never outlives its Impl while it is used. C++14.

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/hub/HostSlots.h>
#include <vprenderer/RendererExtension.h>

#include <memory>

namespace Vpv
{
	namespace Capture
	{
		namespace RE = RendererExtension;

		class ICaptureHost
		{
		public:
			virtual ~ICaptureHost() = default;
			// Lifecycle SettingsApplied (the settings thread that applies them; the renderer's activeSettings is the
			// applied set): the live capture switches (item f).
			virtual void ApplySettings() = 0;
			// Lifecycle GpuReleased (the Retire path, before the device is destroyed): the unpackers, the RGB luma
			// and the early-upload pool (item d).
			virtual void ReleaseGpu() = 0;
			// R16 (conversion thread, no queue lock): the capture route of one frame; true = handled (a payload or
			// an error was produced), false = the host's own formatter.
			virtual bool Convert(RE::ConvertContext& cc) = 0;
			// R16 AfterPublish (conversion thread, after the notify): the early upload of a published raw payload.
			virtual void AfterPublish(RE::PublishView& pv) = 0;
			// The renderer's "Fallbacks engaged" entry (no silent fallbacks): HostBinding raises
			// FallbackCatalog::CaptureUnbound when the renderer could not be registered.
			virtual void SyncFallback(const FallbackCatalog::Decision& decision) = 0;
		};

		template<class Impl>
		class CaptureImplHost : public ICaptureHost
		{
		public:
			explicit CaptureImplHost(Impl& impl) : impl_(impl) {}
			void ApplySettings() override { impl_.CaptureApplySettings(); }
			void ReleaseGpu() override { impl_.CaptureReleaseGpu(); }
			bool Convert(RE::ConvertContext& cc) override { return impl_.CaptureConvert(cc); }
			void AfterPublish(RE::PublishView& pv) override { impl_.CaptureAfterPublish(pv); }
			void SyncFallback(const FallbackCatalog::Decision& decision) override { impl_.SyncFallback(decision); }

		private:
			Impl& impl_;
		};

		template<class Impl>
		std::unique_ptr<ICaptureHost> MakeCaptureHost(Impl& impl) { return std::unique_ptr<ICaptureHost>(new CaptureImplHost<Impl>(impl)); }

		// One registered renderer: the key and its adapter.
		struct HostEntry
		{
			const void* host = nullptr;
			std::unique_ptr<ICaptureHost> view;
		};

		class HostBinding
		{
		public:
			HostBinding(const void* host, std::unique_ptr<ICaptureHost> view);
			~HostBinding();
			HostBinding(const HostBinding&) = delete;
			HostBinding& operator=(const HostBinding&) = delete;
			bool Registered() const { return registered_; }

		private:
			const void* host_ = nullptr;
			bool registered_ = false;
		};

		// The registered adapter of `host` (null: none; the hook then leaves the host's own code to run).
		std::shared_ptr<HostEntry> Find(const void* host);
		// Registered renderers (tests / diagnostics).
		size_t BoundHosts();
	}
}
