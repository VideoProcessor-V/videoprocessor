/*
 * Copyright(C) 2021 Dennis Fleurbaaij <mail@dennisfleurbaaij.com>
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. If not, see < https://www.gnu.org/licenses/>.
 */

#pragma once


#include <atlstr.h>


enum class ColorSpace
{
	UNKNOWN,
	REC_601_525,  // NTSC SMPTE C
	REC_601_576,  // PAL/SECAM (equal to below but used in analog transmission)
	REC_601_625,  // PAL/SECAM (equal to above but used in digital transmission)
	REC_709,
	P3_D65,
	P3_DCI,
	P3_D60,
	BT_2020
};


const TCHAR* ToString(const ColorSpace);

// Convert color sapce to Cie1931 coordinates
double ColorSpaceToCie1931RedX(ColorSpace);
double ColorSpaceToCie1931RedY(ColorSpace);
double ColorSpaceToCie1931GreenX(ColorSpace);
double ColorSpaceToCie1931GreenY(ColorSpace);
double ColorSpaceToCie1931BlueX(ColorSpace);
double ColorSpaceToCie1931BlueY(ColorSpace);
double ColorSpaceToCie1931WpX(ColorSpace);
double ColorSpaceToCie1931WpY(ColorSpace);

// Hunt 2 (08 #4): colour mapping for capture sources flagged BT.601 at any
// picture height. The flag decides the matrix (BT.601); the line count only
// picks between the two BT.601 entries, which differ in their primaries:
// 576/625 lines -> REC_601_625 (EBU), every other height -> REC_601_525
// (SMPTE 170M, the colorimetry HDMI's AVI InfoFrame names for C = 01, so an
// HD source declaring "SMPTE 170M / BT.601" keeps the standard it declared).
// Integration 2026-10-17: rasters other than 480/486/525 and 576/625 choose
// the entry from the measured frame-rate family (25 Hz family -> 625, every
// other rate or no rate -> 525), the same rule DeckLink's Translate uses.
ColorSpace Rec601ColorSpace(unsigned int activeLines, double frameRateHz = 0.0);

// True for an integral rate that is a multiple of 25 but not of 24 or 30
// (25, 50, 100 Hz ...). 1000/1001 rates, 0 and NaN are not.
bool RateIs625Family(double frameRateHz);

// CTA-861 AVI InfoFrame colorimetry: C (data byte 2, bits 7:6) and EC (data
// byte 3, bits 6:4, read when C = 11).
enum class AviColorimetry
{
	NoData,   // C = 00: the CTA-861 default applies (SD: BT.601, HD: BT.709)
	Bt601,    // SMPTE 170M, xvYCC601, sYCC601, opYCC601
	Bt709,    // BT.709, xvYCC709
	Bt2020,   // BT.2020 (constant or non-constant luminance)
	Other,    // opRGB, reserved values
};
AviColorimetry DecodeAviColorimetry(unsigned int c, unsigned int ec);
