// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The camera extension: the H1 adapter of the camera link core (link/README.md "Wiring"). camera_link = report creates, per
// renderer, a CameraLinkCore over that renderer's FrameAnalysis::Store (found through SourceFrameTap::StoreFor), hands it the
// GPU, registers its Indicator reader through a FrameSeal provider and calls its frame phases. Off (the default) and the
// Original pathway: no state, no line.
//
// CAMERA LIVE (default off; nothing below exists unless the keys ask for it):
//   * camera_link = report AND camera_model = on: the fit is wanted (p7Wanted). The extension finds the renderer's vector engine
//     through Query("vpv.vector_engine.v1") -> IVectorEngineDirectory::EngineFor(host). At the first verified field it builds the lag
//     bridge (CameraLane.h: the lagged seal of P7 / P8 until SK-W2), attaches the core and the label fill to it through LEASES
//     (AttachLane) and feeds the fit from the engine's seed fields. NO skeleton demand is declared until SK-W2 (the skeleton would
//     seal P7 / P8 absent every frame and run its engine feed for nobody); LiveCameraDemand is the list SK-W2 declares. An absent engine is refused VISIBLY (Fallbacks "camera_engine"), never silently.
//   * ... AND any camera_meas_* switch on (K3's keys, [vpv.camera]): a CameraGuidanceAdapter is attached to the engine, which
//     registers the ONE pre-estimate hook (P5a's call site; nothing in the renderer changes). All switches off: no adapter, no hook.
//   * Threading: everything runs on the render thread (OnLifecycle, OnFrame). The adapter's detach order: hook cleared first,
//     fit-done observer next (it waits for a call in flight), then the core's worker is joined and the table dropped; the engine
//     pointer is never kept past GpuReleased / Shutdown / a settings change that turns the feature off.
// Status: the Ctrl+I line, Config status, Fallbacks and Ctrl+Alt+D all come from CameraStatus.h plus the guidance lines.
//
// K3 (diagnostics): the [vpv.camera] keys (CameraSettings.h) are read here too. Any key set shows its state through
// CameraStatus.h (Ctrl+I line, Config status, Fallbacks, Ctrl+Alt+D) even when camera_link is off: a switch that cannot act is
// shown, never silent. VP_CAMERA_DUMP=<dir> starts the dump writer (CameraDump.h) on the core, only then a thread exists.

#include "CameraSettings.h"

#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/core/hub/HostSlots.h>
#include <VP-V/modules/control/skeleton/SkeletonSettings.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace CameraLink { class CameraLinkCore; class CameraDump; class CameraGuidanceAdapter; class CameraLane; }

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
			// The standing status while the lag bridge carries the fit (camera-live step 2, until SK-W2): the Ctrl+I line "camera: <text>" and
			// the Fallbacks entry "camera_lag_bridge: <text>". Never log-only.
			static constexpr const char* kLagBridgeId = "camera_lag_bridge";
			static std::string LagBridgeText();

			// Where the renderer's vector engines are looked up; empty = the hub's Query("vpv.vector_engine.v1") (tests pass a fake).
			using EngineDirectoryProvider = std::function<VpMotion::IVectorEngineDirectory*()>;

			explicit CameraExtension(Skeleton::ConfigLoader loader = Skeleton::ConfigLoader(),
				EngineDirectoryProvider engines = EngineDirectoryProvider());
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
			CameraSettings CameraSettingsOf(const void* host) const;   // the [vpv.camera] settings applied to the host (defaults when none)
			const CameraLink::CameraDump* DumpOf(const void* host) const;   // the VP_CAMERA_DUMP writer (null: none)
			// Camera live (render thread / single-threaded tests).
			bool FitWanted(const void* host) const;          // camera_link report + camera_model on
			bool LiveBuilt(const void* host) const;          // the product table exists and the core is attached to it
			const CameraLink::CameraLane* LaneOf(const void* host) const;
			CameraLink::CameraGuidanceAdapter* AdapterOf(const void* host) const;   // null: no measurement switch on, or not live yet
			std::string EngineProblemOf(const void* host) const;   // "" when the engine was found (or not asked for)
			// GUIDANCE LOCK (owner, reset 1): every camera guidance switch (camera_meas_*) is forced off until the on-time rule (d = 0, the
			// camera of frame N guides only N's matcher and only when ready before it) is built next reset; a late camera never guides.
			// The switches parse and are shown ("camera guidance waits for the on-time rule"); no adapter, no hook. Tests of the wiring the
			// next reset unlocks call SetGuidanceLockForTesting(false) BEFORE the first lifecycle call.
			static constexpr const char* kGuidanceLockedText = "camera guidance waits for the on-time rule (next reset): the camera_meas_* switches are off";
			void SetGuidanceLockForTesting(bool locked) { guidanceLocked_ = locked; }
			bool GuidanceLocked() const { return guidanceLocked_; }

		private:
			void Reconcile(const void* host, int originalFlag, pl_gpu gpu);
			void Teardown(const std::shared_ptr<CameraHostState>& st, bool releaseSlot);
			void Refresh(CameraHostState& st);
			// Camera live (render thread). ReleaseLive undoes EnsureLive in the detach order the header comment states.
			VpMotion::IVectorEngine* FindEngine(const void* host) const;
			bool EnsureLive(CameraHostState& st, const void* host, const std::shared_ptr<VpIndicator::Indicator>& indicator);
			void ReleaseLive(CameraHostState& st);
			void DriveFrame(CameraHostState& st, const void* host, int64_t seq);

			Skeleton::ConfigLoader loader_;
			EngineDirectoryProvider engines_;
			Hub::HostSlots<CameraHostState> hosts_;
			bool guidanceLocked_ = true;
		};

		// Adds CameraExtension to the hub. Start-up only; idempotent. After SkeletonModule.
		void RegisterCameraExtension();
	}
}
