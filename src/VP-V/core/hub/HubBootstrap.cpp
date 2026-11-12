// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// Explicit registration of the skeleton hub (M2 D-g). This translation unit is kept by
// /INCLUDE:VpvHubBootstrapAnchor in the renderer projects' items props, so the registration cannot be
// dropped by the linker and costs no line in the host files. It runs at module load, before any
// renderer, capture or file thread exists.

#include "ExtensionHub.h"

#include <VP-V/modules/control/config/VpvSettingsModule.h>
#include <VP-V/modules/control/skeleton/SkeletonModule.h>
#include <VP-V/modules/input/capture/CaptureExtension.h>
#include <VP-V/modules/input/queue_timing/IngressPolicy.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraExtension.h>
#include <VP-V/modules/motion/estimation/VectorEngineExtension.h>
#include <VP-V/modules/motion/consumers/DejudderExtension.h>
#include <VP-V/modules/output/presentation/PresenterExtension.h>
// Last: DiagnosticOverrides.h includes <windows.h> without NOMINMAX; the headers above use (std::min/max) forms that its macros break.
#include <VP-V/modules/control/diagnostics/DiagnosticOverridesModule.h>

namespace
{
	int RegisterAtLoad()
	{
		// The settings module serves the host's settings-load hooks (M2 P3); the hub order does not matter to it.
		Vpv::Settings::RegisterSettingsModule();
		// Fixed fan-out order (doc 63 section 1): resync / FrameSeal first, so every later module
		// sees the frame already sealed.
		Vpv::Resync::RegisterResyncExtension();
		// The capture routes (M2 P10): the conversion route, the early upload after the publish, the live capture
		// switches and the unpackers' GPU release, for the renderer registered under the hook's host.
		Vpv::Capture::RegisterCaptureExtension();
		// R15 DecideIngress (M2 P10-5): pulldown detection, the film clock and the stale-delivery gap of the renderer
		// registered under the hook's host.
		Vpv::Ingress::RegisterIngressPolicy();
		// The vector engine (M2 P5a): answers vpv.vector_engine.v1, turns the queue events into motion invalidation
		// requests (OnQueue) and stops the optical-flow producer at GpuReleased; before its consumers.
		VpMotion::RegisterVectorEngineExtension();
		// The presentation module (Phase 3, refs/108): the R11 main-present interval, the R12 render-ahead step and
		// the R14 present end. Before the skeleton module: at R12 the Presenter's step ran before the skeleton's
		// post-present spare in the host's own order.
		Vpv::Presentation::RegisterPresenterExtension();
		// Activation (all off by default, [vpv.skeleton]): the analysis lane, then the camera link (report).
		Vpv::Skeleton::RegisterSkeletonModule();
		Vpv::Camera::RegisterCameraExtension();
		// The de-judder / per-refresh timeline (M2 P5b): after the vector engine and the camera link, before the
		// entrance and core segments (doc 59 2.2 list order); its views, PlanOutputs and the QueueFlush steering reset.
		VpMotion::RegisterDejudderExtension();
		Vpv::Diagnostics::RegisterDiagnosticOverridesModule();   // status only: the diagnostic override line
		Vpv::Hub::RegisterHub();
		return 1;
	}
}

extern "C" int VpvHubBootstrapAnchor = RegisterAtLoad();
