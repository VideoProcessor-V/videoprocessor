// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5a-5: the vector engine's hub module. It answers Query("vpv.vector_engine.v1") (VpMotion::kVectorEngineQueryId)
// with the IVectorEngineDirectory of the renderers' engines (VectorEngine::Directory: each renderer's VectorEngine
// registers itself under its host key); the consumer (the camera link) asks it for its host's IVectorEngine.
// P5a-rest (items j, k; doc 52 R9): OnQueue turns the renderer's queue events into the one vectors-valid state's
// invalidation requests, with the causes the direct calls used (Reset = resync, LiveQueueReset = live queue reset,
// Gap = frame gap with its detail text, Discontinuity = file seek / step / rate change, FormatChange = source format
// change); any thread, no state of its own (the engine directory keys by host). These requests run with every
// VP-V motion feature off (the epochs are always kept) but NOT in the Original pathway (PHASE 1 of the host seams
// plan, refs/107 s2.4 item 1, owner E3 2026-11-11: a call site in Bill's file does nothing there; the hub's per-host
// pathway fact gates OnQueue, `// VPV-GATED`); leaving Original requests ONE Resync invalidation at that
// SettingsApplied, so nothing from before the Original stretch is read as current.
// DEPENDENCY: these invalidations (and the GPU-release producer stop) now happen only while this module is
// registered in the hub, i.e. only when HubBootstrap.cpp is linked in (/INCLUDE:VpvHubBootstrapAnchor in the
// renderer projects' items props). A renderer built without it, or a test that clears the registry
// (RendererExtension::ResetForTesting), raises the queue events into nothing: no motion invalidation at Reset,
// ResetLiveQueue, gap, file discontinuity or format change.
// No per-frame work. Registered by HubBootstrap right after ResyncExtension (doc 52 3.2 list order).
#pragma once

#include <VP-V/core/hub/ExtensionHub.h>

namespace VpMotion
{
	class VectorEngineExtension : public Vpv::Hub::ISkeletonModule
	{
	public:
		static constexpr const char* kName = "vector_engine";
		const char* Name() const override { return kName; }
		void* Query(const char* iid) override;
		void OnQueue(const RendererExtension::QueueFacts& f) override;   // VPV-GATED (body in the .cpp)
		// P5a-rest (item l): GpuReleased stops this host's optical-flow producer (VectorEngine::ReleaseProducerFor).
		// PHASE 1: SettingsApplied leaving the Original pathway requests one Resync invalidation (never gated: teardown
		// and the hand-over must run in both pathways).
		void OnLifecycle(RendererExtension::LifecycleFacts& f) override;
	};

	// Adds VectorEngineExtension to the hub. Start-up only; idempotent.
	void RegisterVectorEngineExtension();
}
