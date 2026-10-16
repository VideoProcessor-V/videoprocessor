/*
 * Copyright(C) 2021 Dennis Fleurbaaij <mail@dennisfleurbaaij.com>
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. If not, see < https://www.gnu.org/licenses/>.
 */

#pragma once


#include <DeckLinkAPI_h.h>

#include <ColorFormat.h>
#include <BitDepth.h>
#include <VideoFrameEncoding.h>
#include <EOTF.h>
#include <ColorSpace.h>
#include <DisplayMode.h>
#include <ConfigFile.h>

#include <functional>
#include <string>


struct DeckLinkCaptureFormatPreferences
{
	BMDPixelFormat rgb8 = bmdFormat8BitARGB;
	BMDPixelFormat rgb10 = bmdFormat10BitRGB;
	BMDPixelFormat rgb12 = bmdFormat12BitRGB;
	// Fix under test (owner 2026-10-15): a signal the driver reports as
	// 12-bit YCbCr 4:2:2 (the SDK has no 12-bit YCbCr capture format) was
	// refused. [decklink] ycbcr_12bit_capture:
	//   rgb12 (default)  12-bit RGB from the card (all 12 bits kept; the card
	//                    converts the colour), 10-bit 4:2:2 if the card cannot
	//   v210             10-bit 4:2:2 (the card drops the lowest two bits)
	//   off              refused, as before
	enum class YCbCr12Capture { Rgb12, V210, Off };
	YCbCr12Capture ycbcr12 = YCbCr12Capture::Rgb12;
};

// A 12-bit YCbCr 4:2:2 signal (not also RGB).
bool DeckLinkIsYCbCr12(BMDDetectedVideoInputFormatFlags flags);
// The flags the format choice uses for such a signal: as 12-bit RGB 4:4:4
// (rgb12) or 10-bit 4:2:2 (v210); other signals unchanged.
BMDDetectedVideoInputFormatFlags DeckLinkYCbCr12AsRgb12(BMDDetectedVideoInputFormatFlags flags);
BMDDetectedVideoInputFormatFlags DeckLinkYCbCr12AsV210(BMDDetectedVideoInputFormatFlags flags);

bool ReadDeckLinkCaptureFormatPreferences(const ConfigFile& config,
	DeckLinkCaptureFormatPreferences& preferences, std::string& error);

BMDPixelFormat CanonicalDeckLinkCapturePixelFormat(
	BMDDetectedVideoInputFormatFlags detectedSignalFlags);

BMDPixelFormat PreferredDeckLinkCapturePixelFormat(
	BMDDetectedVideoInputFormatFlags detectedSignalFlags,
	const DeckLinkCaptureFormatPreferences& preferences);

BMDPixelFormat ResolveDeckLinkCapturePixelFormat(
	BMDDetectedVideoInputFormatFlags detectedSignalFlags,
	const DeckLinkCaptureFormatPreferences& preferences,
	const std::function<bool(BMDPixelFormat)>& isSupported,
	bool& usedFallback);

const char* DeckLinkPixelFormatName(BMDPixelFormat pixelFormat);


ColorFormat TranslateColorFormat(BMDDetectedVideoInputFormatFlags detectedVideoInputFormatFlagsValue);

BitDepth TranslateBithDepth(BMDDetectedVideoInputFormatFlags detectedVideoInputFormatFlagsValue);

VideoFrameEncoding Translate(BMDPixelFormat, ColorSpace);

EOTF TranslateEOTF(LONGLONG electroOpticalTransferFuncValue);

ColorSpace Translate(BMDColorspace, uint32_t verticalLines);

DisplayModeSharedPtr Translate(BMDDisplayMode, BMDFieldDominance dominance=bmdUnknownFieldDominance);

double FPS(BMDDisplayMode displayMode);
