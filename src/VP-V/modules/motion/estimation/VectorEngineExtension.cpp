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
	void VectorEngineExtension::OnQueue(const RE::QueueFacts& f)
	{
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

	void RegisterVectorEngineExtension()
	{
		static bool registered = false;
		if (registered)
			return;
		registered = true;
		Vpv::Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new VectorEngineExtension()));
	}
}
