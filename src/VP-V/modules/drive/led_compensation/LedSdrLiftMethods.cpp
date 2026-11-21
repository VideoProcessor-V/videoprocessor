// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <pch.h>
#include "LedSdrLiftMethods.h"

#include <DebugLog.h>
#include <vprenderer/LibplaceboRenderParameters.h>

#include <cstdio>
#include <string>

namespace Vpv
{
	namespace LedSdrLift
	{
		void UpdateAmbientSdrLift(LedSdrLiftState& sdrLift, double sdrBlackNits, double sdrTargetNits,
			double renderBlackNits, bool inputIsSdr, enum pl_color_transfer transfer)
		{
			// Hunt 3 F2: for SDR input the target black above stays the SDR reference (sdr_black_nits
			// is not applied to SDR: the signal is display-referred, CONFIGURATION.html), so ambient
			// adaptation's rise of the black reaches the picture as the BT.1886 black lift it stands
			// for (LibplaceboRenderParameters::SdrAmbientLift, through the frame's colour adjustment).
			// Off, strength 0 or a dark room: renderBlackNits is the owner's black exactly and
			// nothing changes (bit-identical).
			float contrast = 1.0f, brightness = 0.0f;
			sdrLift.ambientSdrLift = inputIsSdr && LibplaceboRenderParameters::SdrAmbientLift(sdrBlackNits, renderBlackNits,
				sdrTargetNits, transfer, contrast, brightness);
			sdrLift.ambientSdrContrast = sdrLift.ambientSdrLift ? contrast : 1.0f;
			sdrLift.ambientSdrBrightness = sdrLift.ambientSdrLift ? brightness : 0.0f;
			std::string state;
			char text[256];
			if (sdrLift.ambientSdrLift)
			{
				snprintf(text, sizeof(text), "Ambient adaptation (SDR input): BT.1886 black lift, contrast %.6f brightness %.6f "
					"(effective black %.5f nits over the owner's %.5f, white %.1f nits)", sdrLift.ambientSdrContrast, sdrLift.ambientSdrBrightness,
					renderBlackNits, sdrBlackNits, sdrTargetNits);
				state = text;
			}
			else if (inputIsSdr && renderBlackNits > sdrBlackNits)
			{
				snprintf(text, sizeof(text), "Ambient adaptation (SDR input): not applied (source transfer %s is not a power law)",
					pl_color_transfer_name(transfer));
				state = text;
			}
			else if (!sdrLift.ambientSdrLogged.empty())
				state = "Ambient adaptation (SDR input): no lift (the owner's black)";
			if (!state.empty() && state != sdrLift.ambientSdrLogged)
			{
				sdrLift.ambientSdrLogged = state;
				DebugLog::Log("%s", state.c_str());
			}
		}

		// The gamma an SDR output transfer encodes for: the gamma boost's base
		// (delta 4). Pure powers as given; BT.1886 as 2.4 and sRGB as 2.2; anything
		// else (HDR) keeps 2.2, the value the boost has always used.
		double DisplayGamma(enum pl_color_transfer transfer)
		{
			switch (transfer)
			{
			case PL_COLOR_TRC_GAMMA18: return 1.8;
			case PL_COLOR_TRC_GAMMA20: return 2.0;
			case PL_COLOR_TRC_GAMMA22: return 2.2;
			case PL_COLOR_TRC_GAMMA24: return 2.4;
			case PL_COLOR_TRC_GAMMA26: return 2.6;
			case PL_COLOR_TRC_GAMMA28: return 2.8;
			case PL_COLOR_TRC_BT_1886: return 2.4;
			case PL_COLOR_TRC_SRGB: return 2.2;
			default: return 2.2;
			}
		}
	}
}
