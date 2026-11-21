// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// L-CORE deliverable 1 (lead8/iface-L-CORE.md s2; coordinator OD 2026-11-14 04:44 option B, no Bill line): how VP-V module
// code reaches the skeleton memo of the frame it works on, without including the skeleton module or adding a host call.
//
//   auto fm = Vpv::Skeleton::CurrentFrameMemo();          // render thread (a hook's Run(), a render-phase consumer)
//   if (fm.Valid()) { auto r = fm.memo->Acquire(id, fm.seq, form, key); ... }
//   else            { visible "unavailable" (never a private path) }
//
//   auto fm = Vpv::Skeleton::MemoForHost(host, seq);       // a producer thread that knows its renderer and frame
//
// Empty FrameMemo = no skeleton module, the Original pathway, no frame recorded on this thread, or an unknown renderer.
//
// The SCENE PRODUCT the same way (owner 2026-11-14 17:38 "Ask the skeleton for it same as everything else"; C5 x5dup MUST 1):
//
//   FrameAnalysis::Scene s; const char* why = nullptr;
//   if (Vpv::Skeleton::SceneGroupOf(host, seq, s, &why)) { ... }   // render thread: get-or-compute-and-publish (SkeletonModule::SceneOf)
//   else                                                    { visible "unavailable" with `why` }
//
// It reads the store's scene group, else classifies the frame now and publishes it, so the answer is the same with
// frame_analysis_store off. A consumer never peeks the store for its CURRENT frame's scene.

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/core/hub/CurrentFrame.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>

#include <cstdint>

namespace Vpv
{
	namespace Skeleton
	{
		// Demand() (the skeleton module's demand entry) is in SkeletonFacts.h, through the hub's query slot.

		inline FrameMemo MemoForHost(const void* host, uint64_t seq)
		{
			IExternalDemand* d = host != nullptr ? Demand() : nullptr;
			return d != nullptr ? d->MemoFor(host, seq) : FrameMemo();
		}

		// The memo of the frame THIS thread is rendering (Hub::ThisThreadFrame), with nextSeq when the Entrance phase gave it.
		inline FrameMemo CurrentFrameMemo()
		{
			const Hub::CurrentFrame f = Hub::ThisThreadFrame();
			if (!f.Valid())
				return FrameMemo();
			FrameMemo fm = MemoForHost(f.host, static_cast<uint64_t>(f.seq));
			if (fm.Valid() && !fm.haveNext && f.nextSeq >= 0)
			{
				fm.nextSeq = static_cast<uint64_t>(f.nextSeq);
				fm.haveNext = true;
			}
			return fm;
		}

		// The scene product of frame `seq` of `host` (SkeletonModule::SceneOf through IExternalDemand). known = false: no skeleton
		// module / Original pathway, an unknown renderer, or the frame has no sealed pair of P4 records to classify.
		inline SceneAnswer SceneOf(const void* host, uint64_t seq)
		{
			IExternalDemand* d = host != nullptr ? Demand() : nullptr;
			return d != nullptr ? d->SceneOf(host, seq) : SceneAnswer();
		}

		// The same answer in the store's scene-group form (FrameAnalysis::Scene), for consumers whose rule is written on it. `origin`
		// is not carried by the answer and stays None. False = not answered; `why` (optional) gets the visible cause.
		inline bool SceneGroupOf(const void* host, uint64_t seq, FrameAnalysis::Scene& out, const char** why = nullptr)
		{
			IExternalDemand* d = host != nullptr ? Demand() : nullptr;
			if (d == nullptr)
			{
				if (why)
					*why = host != nullptr ? "no skeleton module (Original pathway): no scene product" : "no renderer frame on this thread: no scene product";
				return false;
			}
			const SceneAnswer a = d->SceneOf(host, seq);
			if (!a.known)
			{
				// S12: the skeleton's own reason (an AcquireCause token, "no analysis lane", "unknown renderer", ...) when it gave one.
				if (why)
					*why = (a.why != nullptr && a.why[0] != 0) ? a.why
						: "the skeleton could not classify the frame (no sealed pair of P4 records: analysis lane off, unknown renderer or not primed yet)";
				return false;
			}
			out = FrameAnalysis::Scene();
			out.cut = a.cut;
			out.fade = a.fade;
			out.cutLate = a.cutLate;
			out.cutHere = a.cutHere;
			out.confirmed = a.confirmed;
			out.flash = a.flash;
			out.cutPossible = a.cutPossible;
			out.cutStrength = a.cutStrength;
			out.fadeKeep = a.fadeKeep;
			return true;
		}
	}
}
