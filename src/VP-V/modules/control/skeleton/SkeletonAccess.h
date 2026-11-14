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

#include <VP-V/core/hub/CurrentFrame.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>

#include <cstdint>

namespace Vpv
{
	namespace Skeleton
	{
		// The skeleton module's demand entry (null without it). One module-list scan.
		inline IExternalDemand* Demand()
		{
			return static_cast<IExternalDemand*>(Hub::ExtensionHub::Instance().Query(kExternalDemandQueryId));
		}

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
	}
}
