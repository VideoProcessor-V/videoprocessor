// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P5b-2 (doc 59 section 2): the de-judder's hub module. Hub order (HubBootstrap): Resync -> VectorEngine
// -> Camera -> Dejudder (-> Entrance -> CoreSegments, P4).
//
// The renderer registers ONE HostBinding (its Impl, keyed by the host pointer its hook calls pass) with:
//   - the views (IDejudder, IPhaseTimeline) answered by Query("vpv.dejudder.v1" / "vpv.phase_timeline.v1")
//     through IDejudderDirectory;
//   - planOutputs (P5b-5, R10 PlanOutputs): the per-vsync timeline plan of a dequeued frame. Render loop
//     thread, renderMutex HELD (P5b-1 correction of doc 59 2.2);
//   - queueFlush (P5b-5, OnQueue(BacklogFlush)): the backlog flush that restarts the timeline's queue steering.
//     Today's semantics: the flag is set at the flush and taken (exchange) by the next frame's plan.
// No renderer registered under a host: nothing is planned (the host's own output list stands) and the
// views are null. DEPENDENCY: like the other hub modules, active only when HubBootstrap is linked in.
// C++14.

#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/motion/consumers/DejudderInterface.h>

namespace VpMotion
{
	namespace RE = RendererExtension;

	using DejudderPlanFn = bool (*)(void* self, RE::OutputPlan& plan);
	using DejudderFlushFn = void (*)(void* self);

	struct DejudderBindingSpec
	{
		IDejudder* dejudder = nullptr;
		IPhaseTimeline* phaseTimeline = nullptr;
		DejudderPlanFn planOutputs = nullptr;
		DejudderFlushFn queueFlush = nullptr;
		void* self = nullptr;
	};

	class DejudderHostBinding
	{
	public:
		DejudderHostBinding(const void* host, const DejudderBindingSpec& spec);
		~DejudderHostBinding();
		DejudderHostBinding(const DejudderHostBinding&) = delete;
		DejudderHostBinding& operator=(const DejudderHostBinding&) = delete;
		bool Registered() const { return registered_; }

	private:
		const void* host_ = nullptr;
		bool registered_ = false;
	};

	class DejudderExtension : public Vpv::Hub::ISkeletonModule
	{
	public:
		static constexpr const char* kName = "motion.dejudder";
		const char* Name() const override { return kName; }
		void* Query(const char* iid) override;
		void OnQueue(const RE::QueueFacts& f) override;
		bool OnPlanOutputs(RE::FrameContext& c, RE::OutputPlan& plan) override;
	};

	// Adds DejudderExtension to the hub. Start-up only; idempotent.
	void RegisterDejudderExtension();
}
