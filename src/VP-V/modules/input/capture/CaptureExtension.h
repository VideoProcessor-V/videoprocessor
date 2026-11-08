// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P10 (doc 59 section 5): the input.capture module of the VP-V ExtensionHub.
//
// It serves the capture hooks of the generic renderer layer for the renderer registered under the hook's host
// (CaptureHost.h): R16 OnConvertedFrame (the capture routes: capture_path gpu, native_rgb_ingest,
// dv_tunnel_gpu_unpack, libplacebo's native RGB upload), R16 AfterPublish (capture_upload early: the upload after
// the publish), Lifecycle SettingsApplied (the live capture switches) and GpuReleased (the unpackers and the
// early-upload pool). The code itself is the renderer's (CaptureImpl.inl); the module only routes.
//
// No renderer registered under the host: every callback does nothing and OnConvertedFrame returns false, so the
// host's own formatter runs (Bill's path). Original pathway: the settings overlay turns the capture switches off
// (capture_path standard, capture_upload render, native_rgb_ingest off, dv_tunnel_gpu_unpack off), so the routes
// return "not handled" except libplacebo's native RGB upload, which the renderer took before P10 as well.
// A throw is contained by the hub (WorkerFault); the conversion route catches its own errors and reports them
// through ConvertContext::error, as the renderer's conversion did.

#include <VP-V/core/hub/ExtensionHub.h>

namespace Vpv
{
	namespace Capture
	{
		namespace RE = RendererExtension;

		class CaptureExtension : public Hub::ISkeletonModule
		{
		public:
			const char* Name() const override { return "input.capture"; }
			void OnLifecycle(RE::LifecycleFacts& f) override;
			bool OnConvertedFrame(RE::FrameContext& c, RE::ConvertContext& cc) override;
			void OnAfterPublish(RE::FrameContext& c, RE::PublishView& pv) override;
		};

		// Start-up only (HubBootstrap); idempotent.
		void RegisterCaptureExtension();
	}
}
