// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>
#include "VectorEngineExtension.h"

#include <VP-V/modules/motion/estimation/MotionValidity.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>

#include <cstring>
#include <memory>

namespace VpMotion
{
	namespace RE = RendererExtension;

	void* VectorEngineExtension::Query(const char* iid)
	{
		if (!iid || std::strcmp(iid, kVectorEngineQueryId) != 0)
			return nullptr;
		return static_cast<void*>(static_cast<IVectorEngineDirectory*>(&VectorEngine::Directory()));
	}

	// Any thread (the event's own: UI for Reset / LiveQueueReset, the render loop's dequeue for the others), in the
	// order the host raises them. Flush and Drop are not motion invalidations here (the drift drop still requests its
	// own directly; the flush comes with a Reset / LiveQueueReset that already did).
	void VectorEngineExtension::OnQueue(const RE::QueueFacts& f)   // VPV-GATED
	{
		// PHASE 1 (refs/107 s2.4 item 1; owner E3): Bill's queue events are not motion invalidations in his pathway.
		const bool originalPathway = Vpv::Hub::ExtensionHub::Instance().OriginalPathway(f.host);
		if (originalPathway) return;
		MotionValidity::Cause cause = MotionValidity::Cause::Resync;
		switch (f.kind)
		{
		case RE::QueueEvent::Reset:          cause = MotionValidity::Cause::Resync; break;
		case RE::QueueEvent::LiveQueueReset: cause = MotionValidity::Cause::QueueReset; break;
		case RE::QueueEvent::Gap:            cause = MotionValidity::Cause::Gap; break;
		case RE::QueueEvent::Discontinuity:  cause = MotionValidity::Cause::Seek; break;
		case RE::QueueEvent::FormatChange:   cause = MotionValidity::Cause::SourceFormatChange; break;
		default: return;
		}
		VectorEngine::RequestInvalidationFor(f.host, cause, f.text);
	}

	// The renderer's GPU release (RetireOutput, before pl_d3d11_destroy; renderMutex held on the Retire path). Fix C
	// (doc 30 F4): the producer no longer outlives the retired output (a kept Impl awaiting an external-state retry
	// held its worker and its device's VRAM indefinitely). Keyed by host (70b section 7). Nothing is logged (as the
	// producer's stop in PlanProducer).
	void VectorEngineExtension::OnLifecycle(RE::LifecycleFacts& f)
	{
		if (f.kind == RE::Lifecycle::GpuReleased)
			VectorEngine::ReleaseProducerFor(f.host);
		// PHASE 1: the settings leave the Original pathway (f.code 0 = VP-V; the hub still answers the PREVIOUS fact during
		// this call): the queue events of the Original stretch were ignored, so one Resync invalidation retires every pair
		// and history from before it. Entering Original needs nothing (Bill's pathway reads no motion state).
		if (f.kind == RE::Lifecycle::SettingsApplied && f.code == 0 && Vpv::Hub::ExtensionHub::Instance().OriginalPathway(f.host))
			VectorEngine::RequestInvalidationFor(f.host, MotionValidity::Cause::Resync, nullptr);
	}

	void RegisterVectorEngineExtension()
	{
		static bool registered = false;
		if (registered)
			return;
		registered = true;
		Vpv::Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new VectorEngineExtension()));
	}
}
