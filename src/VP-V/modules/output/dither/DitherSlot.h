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
	// The sigma-delta's vector producers (the de-judder's vectors on the extras, the per-pixel
	// displacement, B's next-frame vectors, render-ahead's L1 level) run only for a quantiser that
	// can run: the slot on and the libplacebo fork's set-texture call present (modularity sweep
	// 2026-10-25: with the slot off no pass may run for it).
	inline bool VectorsWanted(bool slotRuns, bool setTextureSupported) { return slotRuns && setTextureSupported; }

	// libplacebo's own dither is cleared only for a quantiser in the frame, or for
	// temporal_dither: off while the slot runs. With the slot off the upstream renderer dithers
	// (temporal_dither is a VP key; its Original pathway value is libplacebo).
	inline bool ClearsLibplaceboDither(bool quantiserInFrame, bool temporalDitherOff, bool slotRuns)
	{
		return quantiserInFrame || (temporalDitherOff && slotRuns);
	}

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
