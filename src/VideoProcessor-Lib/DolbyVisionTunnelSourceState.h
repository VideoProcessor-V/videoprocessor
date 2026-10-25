#pragma once
/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// The source state VP gives the renderer for an open DV tunnel (extracted unchanged from
// VideoProcessorDlg, test/dv-synthetic 2026-10-24, so the renderer capture harness runs the
// tunnel exactly as the app does). DeckLink reports the tunnel's wrapper as SDR / Rec.709;
// the recovered picture is PQ / BT.2020, with BT.2020 mastering primaries and 1000-nit
// static defaults wherever the capture's HDR data left them unset. (The renderer replaces
// the luminance range with the DM's source range and zeroes MaxCLL/MaxFALL per picture:
// DolbyVisionLuminancePolicy.h.)
#include <ColorSpace.h>
#include <EOTF.h>
#include <HDRData.h>
#include <VideoState.h>

#include <memory>

inline void ApplyDolbyVisionTunnelSourceState(VideoState& state)
{
	state.eotf = EOTF::PQ;
	state.colorspace = ColorSpace::BT_2020;
	if (!state.hdrData)
		state.hdrData = std::make_shared<HDRData>();
	HDRData& hdr = *state.hdrData;
	if (hdr.displayPrimaryRedX <= 0 || hdr.displayPrimaryGreenX <= 0 ||
		hdr.displayPrimaryBlueX <= 0 || hdr.whitePointX <= 0)
	{
		hdr.displayPrimaryRedX = ColorSpaceToCie1931RedX(ColorSpace::BT_2020);
		hdr.displayPrimaryRedY = ColorSpaceToCie1931RedY(ColorSpace::BT_2020);
		hdr.displayPrimaryGreenX = ColorSpaceToCie1931GreenX(ColorSpace::BT_2020);
		hdr.displayPrimaryGreenY = ColorSpaceToCie1931GreenY(ColorSpace::BT_2020);
		hdr.displayPrimaryBlueX = ColorSpaceToCie1931BlueX(ColorSpace::BT_2020);
		hdr.displayPrimaryBlueY = ColorSpaceToCie1931BlueY(ColorSpace::BT_2020);
		hdr.whitePointX = ColorSpaceToCie1931WpX(ColorSpace::BT_2020);
		hdr.whitePointY = ColorSpaceToCie1931WpY(ColorSpace::BT_2020);
	}
	if (hdr.masteringDisplayMaxLuminance <= 0)
		hdr.masteringDisplayMaxLuminance = 1000;
	if (hdr.maxCll <= 0)
		hdr.maxCll = 1000;
	if (hdr.maxFall <= 0)
		hdr.maxFall = 1000;
}
