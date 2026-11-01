// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>

// One plain sentence per FallbackCatalog id, for the one-time on-screen notice (ADR-0001: no
// silent fallbacks). The ids are the stable names in FallbackCatalog.h (not edited here); the
// catalog's own reasons stay the technical text for Ctrl+I and the log. Header-only, pure, C++14.
namespace FallbackPlainText
{
	// The sentence for an id, or "" when the id is not in the catalog.
	inline const char* Sentence(const std::string& id)
	{
		struct Entry { const char* id; const char* text; };
		static const Entry table[] = {
			{ "dither", "The smooth-gradient dither was replaced by the standard one" },
			{ "BTB/WTW", "Blacker-than-black and whiter-than-white detail is being clipped" },
			{ "re-base", "Picture scaling is using the native frame for now" },
			{ "detail pulse", "The detail stabiliser was skipped for this frame" },
			{ "texture restore", "The texture restore was skipped for this frame" },
			{ "deband", "The standard debander is being used instead of VP's" },
			{ "motion", "The motion engine is not running, so motion-based steps are off" },
			{ "motion consumers", "Motion-based steps are showing the source frame instead" },
			{ "presentation", "Frames are being presented in a simpler way than requested" },
			{ "refresh rate", "The display refresh rate could not be switched as requested" },
			{ "calibration LUT", "The calibration table was switched off after a drawing error" },
			{ "calibration patch", "The calibration patch is drawn full field" },
			{ "output range", "The automatic limited output range is not available" },
			{ "output request", "The requested output format was rejected by the display path" },
			{ "geometry", "The picture is using a safe fit instead of the requested shape" },
			{ "subtitle detection", "Subtitle detection is falling behind and skipping work" },
			{ "SDR tone shape", "The SDR tone shape was not applied" },
			{ "output scaling", "The output scaler was replaced by a simpler one" },
			{ "early upload", "Early upload of frames to the graphics card is off" },
			{ "early upload frames", "Early upload is holding fewer frames than requested" },
			{ "vector_engine", "The motion vector engine is not serving the steps that need it" },
			{ "vector_engine_producer", "The motion vector engine is skipping work to keep up" },
			{ "vector_engine_global", "Global motion detection is not keeping up and is off" },
		};
		for (const Entry& e : table)
			if (id == e.id)
				return e.text;
		return "";
	}

	// The plain sentence plus " (" + short reason + ")" when a reason is given.
	inline std::string FallbackNoticeText(const std::string& id, const std::string& reason)
	{
		const std::string sentence = Sentence(id);
		if (sentence.empty())
			return "A processing step was switched off: " + id;
		return reason.empty() ? sentence : sentence + " (" + reason + ")";
	}
}

using FallbackPlainText::FallbackNoticeText;
