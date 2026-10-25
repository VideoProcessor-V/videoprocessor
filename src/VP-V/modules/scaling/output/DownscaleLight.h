#pragma once

#include <libplacebo/common.h>

#include <cmath>
#include <string>

// Stage 8-1: `downscale_light: linear | gamma` (rendering profile, scaling
// group; default linear = libplacebo's own linear-light scaling). Gamma
// scales a downscaling frame in the encoded (gamma) domain, as the owner's
// madVR ran SSIM, by setting disable_linear_scaling for that frame only:
// upscaling frames keep linear light (and their sigmoid). libplacebo has one
// switch for both axes, so a frame that downscales on either axis counts.
// Both Stage 8 branches read the PREKERNEL picture, so they follow it.
namespace DownscaleLight
{
	inline bool Downscales(const pl_rect2df& source, const pl_rect2df& target)
	{
		return std::fabs(source.x1 - source.x0) > std::fabs(target.x1 - target.x0) + 0.5f ||
			std::fabs(source.y1 - source.y0) > std::fabs(target.y1 - target.y0) + 0.5f;
	}

	inline bool UseGammaLight(const std::string& setting, const pl_rect2df& source, const pl_rect2df& target)
	{
		return setting == "gamma" && Downscales(source, target);
	}
}
