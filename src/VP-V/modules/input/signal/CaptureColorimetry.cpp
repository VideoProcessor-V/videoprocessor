// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pch.h"

#include <ColorSpace.h>

#include <cmath>


bool RateIs625Family(double frameRateHz)
{
	// Classified from the measured rate, no fixed list: an integral rate that
	// is a multiple of 25 but not of 24 or 30 belongs to the 25 Hz family.
	if (!(frameRateHz > 0.0) || !std::isfinite(frameRateHz))
		return false;
	const double rounded = std::round(frameRateHz);
	if (std::fabs(frameRateHz - rounded) > 1e-3)
		return false;   // 1000/1001 rates are the 525 family
	const long long rate = static_cast<long long>(rounded);
	return rate % 25 == 0 && rate % 24 != 0 && rate % 30 != 0;
}


ColorSpace Rec601ColorSpace(unsigned int activeLines, double frameRateHz)
{
	if (activeLines == 576 || activeLines == 625)
		return ColorSpace::REC_601_625;
	if (activeLines == 480 || activeLines == 486 || activeLines == 525)
		return ColorSpace::REC_601_525;
	// Integration 2026-10-17: any other raster (HD/UHD flagged BT.601) takes
	// the primaries from the measured rate family, as the DeckLink path does,
	// so both capture cards agree. No rate known -> 525 (SMPTE 170M).
	return RateIs625Family(frameRateHz) ? ColorSpace::REC_601_625 : ColorSpace::REC_601_525;
}


AviColorimetry DecodeAviColorimetry(unsigned int c, unsigned int ec)
{
	switch (c & 3u)
	{
	case 0: return AviColorimetry::NoData;
	case 1: return AviColorimetry::Bt601;
	case 2: return AviColorimetry::Bt709;
	default: break;
	}
	switch (ec & 7u)
	{
	case 0:   // xvYCC601
	case 2:   // sYCC601
	case 3:   // opYCC601
		return AviColorimetry::Bt601;
	case 1:   // xvYCC709
		return AviColorimetry::Bt709;
	case 5:   // BT.2020 YcCbcCrc
	case 6:   // BT.2020 R'G'B' or Y'C'bC'r
		return AviColorimetry::Bt2020;
	default:  // 4 opRGB, 7 reserved (additional colorimetry extension)
		return AviColorimetry::Other;
	}
}
