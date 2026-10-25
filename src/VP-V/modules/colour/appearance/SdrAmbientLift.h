// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// colour.appearance: the SDR ambient black-lift (Hunt 3 F2), extracted unchanged from
// vprenderer/LibplaceboRenderParameters (M2 step 7). LibplaceboRenderParameters::SdrAmbientLift
// and ::ApplySdrAmbientLift forward here.

#include <libplacebo/colorspace.h>

#include <cmath>

namespace SdrAmbientLiftModule
{
	// Hunt 3 F2: ambient adaptation for SDR input. The equal source/target references in LibplaceboRenderParameters
	// keep SDR codes as they are, so sdr_black_nits never reaches an SDR picture (intended:
	// the signal is display-referred and the display's own calibration owns its black;
	// CONFIGURATION.html). Ambient adaptation's rise of the effective black above the owner's
	// black (the room's veil x strength) is applied for SDR input as the BT.1886 black lift
	// it stands for: on the owner's display (black B, white W, exponent g of the source
	// transfer) the picture is shown as on a display whose black is B_eff:
	//   L(V') = EOTF_B(V') = EOTF_Beff(V)  =>  V' = s V + o,
	//   s = (lw - le) / (lw - lb), o = (le - lb) / (lw - lb), lb = B^(1/g), le = B_eff^(1/g), lw = W^(1/g)
	// - affine in the signal (s + o = 1: white fixed; s > 0: code order kept, BTB stays below
	// black and WTW above white, both carried as values), applied by libplacebo's colour
	// decode (pl_color_adjustment contrast s, brightness o) before anything else.
	// False (nothing changes, bit-identical) when B_eff <= B (ambient off, strength 0, a dark
	// room) or the source transfer is not a power law (BT.1886, gamma 1.8-2.8).

	inline bool SdrAmbientLift(double ownerBlackNits, double effectiveBlackNits, double whiteNits,
		enum pl_color_transfer sourceTransfer, float& contrast, float& brightness)
	{
		// The exponent of the source's power-law curve (BT.1886 is the 2.4 law with the display
		// black in it; the pure gammas are the same form with their own exponent).
		double gamma = 0.0;
		switch (sourceTransfer)
		{
		case PL_COLOR_TRC_BT_1886: gamma = 2.4; break;
		case PL_COLOR_TRC_GAMMA18: gamma = 1.8; break;
		case PL_COLOR_TRC_GAMMA20: gamma = 2.0; break;
		case PL_COLOR_TRC_GAMMA22: gamma = 2.2; break;
		case PL_COLOR_TRC_GAMMA24: gamma = 2.4; break;
		case PL_COLOR_TRC_GAMMA26: gamma = 2.6; break;
		case PL_COLOR_TRC_GAMMA28: gamma = 2.8; break;
		default: return false;
		}
		if (!std::isfinite(ownerBlackNits) || !std::isfinite(effectiveBlackNits) || !std::isfinite(whiteNits) ||
			!(whiteNits > 0.0) || ownerBlackNits < 0.0 || !(effectiveBlackNits > ownerBlackNits) ||
			!(effectiveBlackNits < whiteNits))
			return false;
		const double lw = std::pow(whiteNits, 1.0 / gamma);
		const double lb = std::pow(ownerBlackNits, 1.0 / gamma);
		const double le = std::pow(effectiveBlackNits, 1.0 / gamma);
		if (!(lw > lb))
			return false;
		const double s = (lw - le) / (lw - lb);
		const double o = (le - lb) / (lw - lb);
		if (!(s > 0.0) || !(o > 0.0))
			return false;
		contrast = static_cast<float>(s);
		brightness = static_cast<float>(o);
		return true;
	}

	inline void ApplySdrAmbientLift(float contrast, float brightness, pl_color_adjustment& adjustment)
	{
		// libplacebo's decode: RGB = contrast * M (YCbCr - black) + brightness.
		adjustment.brightness = adjustment.brightness * contrast + brightness;
		adjustment.contrast *= contrast;
	}
}
