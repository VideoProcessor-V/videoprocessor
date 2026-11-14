// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/FallbackCatalog.h>

#include <string>

// S-12BIT (refs/119 #29 / #48 / #49 / #30; owner decision 2026-11-12 15:07): the precision of libplacebo's
// intermediate pictures, decided by the fbo_float32 setting ALONE, and the visible state of it.
//
//  - FboFloat32(setting): auto (-1) and on (1) ask for rgba32f intermediates, off (0) for rgba16f. It no longer
//    depends on whether the scaling.output slot runs: a profile with that slot off used to drop silently to
//    float16 (about 11 bits of significand, up to one 12-bit code of error near white).
//  - Precision(): the "picture precision" fallback entry. Engaged whenever the intermediates are float16 in a VP
//    pathway: the Stable 7.360 lane (its fork has no float32 intermediate switch at all) or a Beta lane with
//    the setting off by hand. Released in the Original pathway (Bill's own table keeps it off by design).
// Pure and CPU-tested: no libplacebo type here.
namespace PicturePrecision
{
	inline bool FboFloat32(int setting) { return setting != 0; }

	enum class Why
	{
		None,
		StableLane,   // libplacebo 7.360: no float32 intermediate or output switch exists
		SettingOff,   // Beta lane, fbo_float32 off
	};

	inline Why Classify(bool originalPathway, bool laneHasFloat32Fbo, int setting)
	{
		if (originalPathway) return Why::None;
		if (!laneHasFloat32Fbo) return Why::StableLane;
		return FboFloat32(setting) ? Why::None : Why::SettingOff;
	}

	inline FallbackCatalog::Decision Precision(bool originalPathway, bool laneHasFloat32Fbo, int setting)
	{
		switch (Classify(originalPathway, laneHasFloat32Fbo, setting))
		{
		case Why::StableLane:
			return FallbackCatalog::Engaged("picture precision",
				"Stable 7.360 libplacebo lane: intermediate pictures are float16, below 12-bit (the Beta lane has float32)");
		case Why::SettingOff:
			return FallbackCatalog::Engaged("picture precision",
				"fbo_float32 is off: intermediate pictures are float16, below 12-bit");
		default:
			return FallbackCatalog::Released("picture precision");
		}
	}
}
