// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P10-5 / P10-6 (doc 59 sections 1.1 R15, R19 and 5.2): the input.queue_timing hub module for R15 DecideIngress and
// R19 ResolveQueueTarget (the renderer's VP-V queue target, QueueTarget.inl).
//
// The renderer's ingress decision (pulldown detection, the film clock, the stale-delivery gap: IngressPolicy.inl) needs
// the renderer's own state, so the renderer registers ONE binding (its last member, keyed by the renderer itself, the
// `host` its DecideIngress call passes) with a plain function pointer to its decision; this module finds it per call.
// No renderer registered under the host: not handled (the host's pre-filled values stand: no pulldown, no stale gap).
// A renderer that could not register (slot table full) is logged and shown under "Fallbacks engaged" (OnStatus).
// A throw in the decision is caught HERE (not left to the hub's containment, which would lose the drop decision
// silently): counted, logged at powers of two, shown under "Fallbacks engaged" (ingress_fault); the frame is queued
// as a no-fill gap without film timing and the renderer restarts its pulldown detector (FaultFn).
//
// Threading: Decide runs on the capture thread and takes no lock: a key compare and one shared_ptr load
// (core/hub/HostSlots.h). The binding is created with the renderer and removed first at its destruction; the capture
// callbacks end before the renderer is destroyed (Stop / StopWithIngressDrain), as for its other members. C++14.

#include <VP-V/core/hub/ExtensionHub.h>

#include <cstdint>

namespace Vpv
{
	namespace Ingress
	{
		namespace RE = RendererExtension;

		using DecideFn = bool (*)(void* self, RE::IngressContext& ic);
		using QueueTargetFn = void (*)(const void* self, RE::QueueTargetContext& qc);   // R19: sets qc.target
		using FaultFn = void (*)(void* self);   // after a contained throw in decide: the renderer restarts its detector

		class HostBinding
		{
		public:
			HostBinding(const void* host, DecideFn decide, void* self, QueueTargetFn queueTarget = nullptr, FaultFn fault = nullptr);
			~HostBinding();
			HostBinding(const HostBinding&) = delete;
			HostBinding& operator=(const HostBinding&) = delete;
			bool Registered() const { return registered_; }

		private:
			const void* host_ = nullptr;
			bool registered_ = false;
			bool unbound_ = false;   // counted in UnboundRenderers() while it lives
		};

		// The registered renderer's decision for `ic` (false: none registered under ic.host).
		bool Decide(RE::IngressContext& ic);
		// R19 (any thread holding the host's queue mutex): the registered renderer's queue target into qc.target.
		void QueueTarget(const void* host, RE::QueueTargetContext& qc);
		size_t BoundHosts();
		size_t UnboundRenderers();   // live renderers that could not register
		uint64_t IngressFaults();    // contained throws in the decisions of the live renderers

		// PHASE 1 of the host seams plan (refs/107 s2.4 item 1; owner E3 2026-11-11): the pathway the renderer's ACTIVE
		// settings select, kept on its binding (the ingress host key is the renderer, not the Impl the hub's
		// SettingsApplied fact is keyed by). The renderer tells it from the first arrival on, when it changes
		// (IngressState::SyncIngressSwitches in IngressPolicy.inl / TimedFileFrames.inl); no binding = VP-V (as before).
		void NoteOriginalPathway(const void* host, bool original);
		bool OriginalPathwayOf(const void* host);

		class IngressPolicy : public Hub::ISkeletonModule
		{
		public:
			const char* Name() const override { return "input.queue_timing"; }
			// Original pathway: not handled, so Bill's pre-filled facts stand (his OnVideoFrame: no pulldown, no stale gap).
			bool OnDecideIngress(RE::FrameContext&, RE::IngressContext& ic) override   // VPV-GATED
			{
				const bool originalPathway = OriginalPathwayOf(ic.host);
				if (originalPathway) return false;
				return Decide(ic);
			}
			// Original pathway: qc.target stays Bill's own value (PrefillTargetLocked's ClampDesiredDepthToCapacity, which
			// BillQueuePolicy::QueueTarget computed from the same inputs before).
			void OnQueueTarget(RE::FrameContext& c, RE::QueueTargetContext& qc) override   // VPV-GATED
			{
				const bool originalPathway = OriginalPathwayOf(c.host);
				if (originalPathway) return;
				QueueTarget(c.host, qc);
			}
			void OnStatus(const RE::Context&, RE::StatusKind kind, std::vector<std::string>& lines, const char*, const char*) override;
		};

		// Start-up only (HubBootstrap); idempotent.
		void RegisterIngressPolicy();
	}
}
