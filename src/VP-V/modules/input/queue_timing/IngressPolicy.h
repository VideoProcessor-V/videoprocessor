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
//
// Threading: Decide runs on the capture thread and takes no lock: a key compare and one shared_ptr load
// (core/hub/HostSlots.h). The binding is created with the renderer and removed first at its destruction; the capture
// callbacks end before the renderer is destroyed (Stop / StopWithIngressDrain), as for its other members. C++14.

#include <VP-V/core/hub/ExtensionHub.h>

namespace Vpv
{
	namespace Ingress
	{
		namespace RE = RendererExtension;

		using DecideFn = bool (*)(void* self, RE::IngressContext& ic);
		using QueueTargetFn = void (*)(const void* self, RE::QueueTargetContext& qc);   // R19: sets qc.target

		class HostBinding
		{
		public:
			HostBinding(const void* host, DecideFn decide, void* self, QueueTargetFn queueTarget = nullptr);
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

		class IngressPolicy : public Hub::ISkeletonModule
		{
		public:
			const char* Name() const override { return "input.queue_timing"; }
			bool OnDecideIngress(RE::FrameContext&, RE::IngressContext& ic) override { return Decide(ic); }
			void OnQueueTarget(RE::FrameContext& c, RE::QueueTargetContext& qc) override { QueueTarget(c.host, qc); }
			void OnStatus(const RE::Context&, RE::StatusKind kind, std::vector<std::string>& lines, const char*, const char*) override;
		};

		// Start-up only (HubBootstrap); idempotent.
		void RegisterIngressPolicy();
	}
}
