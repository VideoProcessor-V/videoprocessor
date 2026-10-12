#pragma once

#include <libplacebo/colorspace.h>

// Delta 5 (D5-1): the code domain the temporal sigma-delta quantises to,
// from the frame's target representation. The shader works in 8- or 10-bit
// codes only; a float or 16-bit target (R16G16B16A16_FLOAT, scRGB) is not
// a domain it can serve: quantising it to 10-bit codes would clip values
// above 1.0 and throw away the float precision (audit delta5-beta.md), so
// the renderer leaves the sigma-delta out for such a frame.
namespace SigmaDeltaDomain
{
	struct Domain
	{
		bool full = false;
		int sample = 8;      // swapchain sample depth
		int color = 8;       // colour depth quantised to
		bool supported = true;
	};

	inline Domain FromTarget(const pl_color_repr& repr)
	{
		Domain d;
		d.full = pl_color_levels_guess(&repr) == PL_COLOR_LEVELS_FULL;
		d.sample = repr.bits.sample_depth > 0 ? repr.bits.sample_depth : 8;
		d.color = repr.bits.color_depth > 0 ? repr.bits.color_depth : d.sample;
		d.supported = d.sample >= 8 && d.sample <= 10 && d.color >= 8 && d.color <= d.sample;
		return d;
	}
}
