// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// S0: the one adapter from the R12 AfterPresent payloads to the pure SlotBudgetFeed's SlotPresentFacts. Option B (lead 8, zero Bill lines): the
// AfterPresent payload carries no render ms and no rates, so the module hands in its own render ms (module clock, biased high = conservative budget)
// and the lane's period (the nominal rate); the measured source rate is cached from the Entrance fan-out (SlotBudgetFeed::NoteCaptureRate).
// Option C (a host seam with the real renderMs) is the later registered upgrade: only the two arguments below change.

#include <vprenderer/RendererExtension.h>
#include <VP-V/modules/control/skeleton/SlotBudgetFeed.h>

namespace Vpv
{
	namespace Skeleton
	{
		inline SlotPresentFacts FactsOf(const RE::FrameContext& c, const RE::PresentContext& p, VpSkeleton::SpareTrigger trigger,
			double moduleRenderMs, double lanePeriodMs)
		{
			SlotPresentFacts f;
			f.sourceSequence = c.seq > 0 ? static_cast<uint64_t>(c.seq) : 0u;
			f.trigger = trigger;
			f.plain = p.kind == RE::AfterPresentKind::Plain;
			f.rendered = p.rendered;
			f.cadenceRepeat = p.cadenceRepeat;
			f.renderMs = moduleRenderMs;
			f.captureRateHz = 0.0;   // the feed uses the Entrance-cached measured rate
			f.nominalRateHz = lanePeriodMs > 0.0 ? 1000.0 / lanePeriodMs : 0.0;
			return f;
		}
	}
}
