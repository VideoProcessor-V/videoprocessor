// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// L-CORE deliverable 1 (lead8/iface-L-CORE.md s1; coordinator OD 2026-11-14 04:44 "skeleton current-frame context", option B:
// no Bill line): the frame a renderer is rendering, recorded by the hub from the EXISTING FrameContext path
// (ExtensionHub::OnFrame, before the fan-out), so VP-V module code that runs inside the render (a hook's Run(), a pass body,
// a render-phase consumer) gets the source seq without any new host call.
//
// Recorded on the render-thread phases whose seq names the frame being rendered (seq >= 0): Begin, PlanOutputs, Entrance,
// CollectHooks, Draw, AfterRender, AfterPresent. Begin starts a new frame (serial + 1; nextSeq unknown until Entrance; the
// generation of the last phase that carried one is kept). Never recorded from Convert, AfterPublish, CollectAhead,
// QueueTarget, DecideIngress, PresentSample, ResolveTime or PlanPresent (other threads or seq -1).
//
//   ThisThreadFrame()      the frame THIS thread renders (sticky thread_local: valid after the phase that set it until the
//                          next one); a thread that never rendered gets Valid() == false.
//   CurrentFrameOf(host)   any thread: the host's last recorded frame (lock-free, never torn; empty for an unknown host or
//                          after its GpuReleased / Shutdown).
// Cost: one thread_local store and one slot lookup with four relaxed stores per recorded phase; nothing else.

#include <vprenderer/RendererExtension.h>

#include <cstdint>

namespace Vpv
{
	namespace Hub
	{
		struct CurrentFrame
		{
			const void* host = nullptr;
			int64_t seq = -1;
			int64_t nextSeq = -1;
			int64_t generation = 0;
			RendererExtension::FramePhase phase = RendererExtension::FramePhase::Begin;
			uint64_t serial = 0;   // + 1 at every Begin of the host (a cadence repeat of one seq is a new serial)
			bool Valid() const { return host != nullptr && seq >= 0; }
		};

		// True for the phases that record the current frame (see above).
		inline bool PhaseRecordsCurrentFrame(RendererExtension::FramePhase p)
		{
			using P = RendererExtension::FramePhase;
			return p == P::Begin || p == P::PlanOutputs || p == P::Entrance || p == P::CollectHooks || p == P::Draw
				|| p == P::AfterRender || p == P::AfterPresent;
		}

		CurrentFrame ThisThreadFrame();
		CurrentFrame CurrentFrameOf(const void* host);
		// The hub's recorder (ExtensionHub::OnFrame; tests). Ignored for a phase that does not record or a null host / seq < 0.
		void RecordCurrentFrame(const RendererExtension::FrameContext& c);
		// Drops the host's record (GpuReleased / Shutdown) and, on this thread, a sticky frame of that host.
		void ForgetCurrentFrame(const void* host);
		// Tests: clears this thread's sticky frame.
		void ResetThisThreadFrameForTesting();
	}
}
