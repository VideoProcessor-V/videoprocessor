#pragma once

// The pipeline slot registry's state for the repeat-reuse key (WP-24, VP-ONLY; modularity review
// 2026-10-25): any slot switched on/off, a component appearing or going, the order, the Original
// pathway or B3 (entrance once) changes the picture before OUTPUT, so it must refuse the reuse even
// when no settings apply moved the settings generation (e.g. a GPU-failure path calling SetEnabled).

#include "RepeatReusePolicy.h"

#include <VP-V/core/SlotRegistry.h>

#include <cstdint>

namespace VpRepeatReuse
{
	inline uint64_t RegistryFingerprint(const PipelineSlots::Registry& registry)
	{
		RepeatRenderReuse::Signature s;
		for (const PipelineSlots::Info& info : PipelineSlots::Slots())
			s.Add(info.id).Add(registry.Enabled(info.id)).Add(registry.HasComponent(info.id)).Add(registry.Order(info.id));
		return s.Add(registry.OriginalPathway()).Add(registry.EntranceOnce()).Value();
	}
}
