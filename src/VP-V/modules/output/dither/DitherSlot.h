#pragma once

#include <libplacebo/renderer.h>

#include <algorithm>
#include <cstddef>
#include <vector>

// output.dither's slot switch (pipeline_slots_off; owner modularity rule 2026-10-25): with the slot
// off the frame carries no output quantiser hook (the sigma-delta), so the renderer keeps
// libplacebo's own dither (its `quantiserInFrame` test then finds none). Byte-identical to a shader
// list without the sigma-delta (temporal_dither: libplacebo). The dither stays LAST whenever it runs
// (the renderer's dither-last step and OutputHookOrder's kQuantiser rank); this only removes it.
// Header-only, pure; render thread.
namespace DitherSlot
{
	// Removes every hook `isQuantiser` accepts, keeping the order of the rest. Returns how many.
	template <class IsQuantiser>
	size_t RemoveQuantisers(std::vector<const pl_hook*>& hooks, IsQuantiser isQuantiser)
	{
		const size_t before = hooks.size();
		hooks.erase(std::remove_if(hooks.begin(), hooks.end(),
			[&](const pl_hook* hook) { return isQuantiser(hook); }), hooks.end());
		return before - hooks.size();
	}
}
