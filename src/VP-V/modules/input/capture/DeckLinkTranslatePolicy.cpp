// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>

#pragma warning(disable : 26812)  // class enum over class in BM API

#include <stdexcept>

#include <DeckLinkAPI_h.h>

#include <blackmagic_decklink/BlackMagicDeckLinkTranslate.h>


namespace
{
	DisplayMode::FieldOrder TranslateFieldOrder(BMDFieldDominance dominance)
	{
		return dominance == bmdUpperFieldFirst ? DisplayMode::FieldOrder::TopFirst :
			dominance == bmdLowerFieldFirst ? DisplayMode::FieldOrder::BottomFirst :
			DisplayMode::FieldOrder::Unknown;
	}
}


bool ParseDeckLinkYCbCr12Capture(const std::string& value,
	DeckLinkCaptureFormatPreferences::YCbCr12Capture& choice)
{
	using Choice = DeckLinkCaptureFormatPreferences::YCbCr12Capture;
	const std::string normalized = ConfigFile::NormalizeName(value);
	if (normalized == "auto")
		choice = DeckLinkCaptureFormatPreferences{}.ycbcr12;   // bug hunt 2: v210
	else if (normalized == "v210")
		choice = Choice::V210;
	else if (normalized == "rgb12")
		choice = Choice::Rgb12;
	else if (normalized == "off")
		choice = Choice::Off;
	else
		return false;
	return true;
}


bool DeckLinkIsYCbCr12(BMDDetectedVideoInputFormatFlags flags)
{
	return (flags & bmdDetectedVideoInputYCbCr422) && (flags & bmdDetectedVideoInput12BitDepth) &&
		!(flags & bmdDetectedVideoInputRGB444);
}


BMDDetectedVideoInputFormatFlags DeckLinkYCbCr12AsRgb12(BMDDetectedVideoInputFormatFlags flags)
{
	if (!DeckLinkIsYCbCr12(flags))
		return flags;
	return static_cast<BMDDetectedVideoInputFormatFlags>(
		(flags & ~bmdDetectedVideoInputYCbCr422) | bmdDetectedVideoInputRGB444);
}


BMDDetectedVideoInputFormatFlags DeckLinkYCbCr12AsV210(BMDDetectedVideoInputFormatFlags flags)
{
	if (!DeckLinkIsYCbCr12(flags))
		return flags;
	return static_cast<BMDDetectedVideoInputFormatFlags>(
		(flags & ~bmdDetectedVideoInput12BitDepth) | bmdDetectedVideoInput10BitDepth);
}


bool DeckLinkRateIs625Family(double frameRateHz)
{
	// One classifier for both capture cards (CaptureColorimetry.cpp).
	return RateIs625Family(frameRateHz);
}


DisplayModeSharedPtr TranslateDeckLinkDisplayMode(BMDDisplayMode displayMode,
	long width, long height, BMDTimeValue frameDuration, BMDTimeScale timeScale,
	BMDFieldDominance dominance)
{
	// audit 2026-10-16: the table stays authoritative for the modes it knows
	// (exact rationals); any other mode is built from what the card reports
	// instead of throwing, so a new card or SDK mode does not stall capture.
	BMDTimeValue tableDuration = 0;
	BMDTimeScale tableScale = 0;
	if (DeckLinkModeTableFrameRate(displayMode, tableDuration, tableScale))
		return Translate(displayMode, dominance);

	if (width <= 0 || height <= 0 || frameDuration <= 0 || timeScale <= 0 ||
		frameDuration > 0xFFFFFFFFLL || timeScale > 0xFFFFFFFFLL)
		throw std::runtime_error("DeckLink display mode is not in the mode table and reports no usable geometry/frame rate");

	// Interlaced modes report the frame (two-field) rate, as the table does.
	const bool interlaced =
		dominance == bmdUpperFieldFirst || dominance == bmdLowerFieldFirst;
	return std::make_shared<DisplayMode>(
		(unsigned int)width,
		(unsigned int)height,
		interlaced,
		(unsigned int)timeScale,
		(unsigned int)frameDuration,
		TranslateFieldOrder(dominance));
}


DisplayModeSharedPtr TranslateDeckLinkDisplayMode(IDeckLinkDisplayMode* mode)
{
	// audit 2026-10-16
	if (!mode)
		throw std::runtime_error("No DeckLink display mode");
	BMDTimeValue frameDuration = 0;
	BMDTimeScale timeScale = 0;
	if (mode->GetFrameRate(&frameDuration, &timeScale) != S_OK)
		frameDuration = timeScale = 0;
	return TranslateDeckLinkDisplayMode(mode->GetDisplayMode(),
		mode->GetWidth(), mode->GetHeight(), frameDuration, timeScale,
		mode->GetFieldDominance());
}
