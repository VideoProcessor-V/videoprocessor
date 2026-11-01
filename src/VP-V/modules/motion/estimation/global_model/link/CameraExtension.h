// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The camera extension: the H1 adapter of the camera link core (link/README.md "Wiring TODO"), in REPORT
// mode only. camera_link = report creates, per renderer, a CameraLinkCore over that renderer's
// FrameAnalysis::Store (found through SourceFrameTap::StoreFor), hands it the GPU, registers its Indicator
// reader through a FrameSeal provider and calls its frame phases. There is no vector engine yet (P5a is
// pending), so the core is configured with p7Wanted = false: no product, ring, pool, producer, thread or
// shader text exists, and the extension never feeds a consumer (no "vpv.camera.v1" answer). What it adds
// is the visible state: a status line and the fallbacks. Off (the default) and the Original pathway:
// no state, no line.

#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/core/hub/HostSlots.h>
#include <VP-V/modules/control/skeleton/SkeletonSettings.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace CameraLink { class CameraLinkCore; }

namespace Vpv
{
	namespace Camera
	{
		namespace RE = RendererExtension;

		struct CameraHostState;

		class CameraExtension : public Hub::ISkeletonModule
		{
		public:
			static constexpr const char* kName = "camera";
			static constexpr const char* kLineKey = "camera";
			static constexpr const char* kReportLine =
				"camera: report mode; fit idle (no vector engine yet, P5a pending); guided matching (K4) off";

			explicit CameraExtension(Skeleton::ConfigLoader loader = Skeleton::ConfigLoader());
			~CameraExtension() override;

			const char* Name() const override { return kName; }
			void OnLifecycle(RE::LifecycleFacts&) override;
			bool OnFrame(RE::FrameContext&) override;
			void OnStatus(const RE::Context&, RE::StatusKind, std::vector<std::string>&, const char*, const char*) override;
			bool OnCommand(const RE::Context&, const char*, const char*, std::string&) override;

			// Diagnostics and tests.
			bool Active(const void* host) const;                    // report mode is on for this renderer
			CameraLink::CameraLinkCore* CoreOf(const void* host) const;   // null without one (render thread / tests)
			size_t HostStates() const;
			const void* StoreOf(const void* host) const;   // the FrameAnalysis::Store the host's core was built over (null: none)

		private:
			void Reconcile(const void* host, int originalFlag, pl_gpu gpu);
			void Teardown(const std::shared_ptr<CameraHostState>& st, bool releaseSlot);
			void Refresh(CameraHostState& st);

			Skeleton::ConfigLoader loader_;
			Hub::HostSlots<CameraHostState> hosts_;
		};

		// Adds CameraExtension to the hub. Start-up only; idempotent. After SkeletonModule.
		void RegisterCameraExtension();
	}
}
