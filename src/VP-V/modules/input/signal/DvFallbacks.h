// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/FallbackCatalog.h>

#include <string>

// The "Fallbacks engaged" entry for lost Dolby Vision metadata (item 16; ADR-0001 no silent
// fallbacks). Header-only, pure, C++14. The renderer passes the adapter's facts
// (VpDvResync::MetadataHolder::Lost / LostFrames) to this builder and hands the Decision to
// FallbackCatalog::Sync, like every other builder in FallbackCatalog.h.
namespace VpDvResync
{
	// Stable id of the entry (the GUI's one-time notice keys on it).
	inline const char* DvMetadataLostId() { return "dv_metadata_lost"; }

	// lost: the picture has gone without Dolby Vision metadata (RPU) for longer than the bounded hold
	// (a time at the measured rate), so the picture is tone mapped from the last metadata's static part
	// (or shown black when none is left). frames: how many pictures in a row went without it.
	inline FallbackCatalog::Decision DvMetadataLost(bool lost, int frames)
	{
		if (!lost)
			return FallbackCatalog::Released(DvMetadataLostId());
		return FallbackCatalog::Engaged(DvMetadataLostId(),
			"no Dolby Vision metadata for " + std::to_string(frames < 0 ? 0 : frames) +
			" pictures: the last metadata's static part is used (no per-scene tone mapping, no trims) until it returns");
	}
}
