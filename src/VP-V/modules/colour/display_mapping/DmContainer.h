// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// colour.display_mapping (CF-1 FIX1 MUST 3): which capture containers VP's DV display mapping can read, and the two
// per-frame rows the host syncs (never silent: refs/125 s5 "DV display mapping: unsupported container"; the LUT moved to
// VP's last step under calibration_lut_stage auto). Pure CPU; the host (CoreSegments.inl) passes plain values.
//
// R12B / R12L are the 12-bit RGB containers: a tunnel picture in them is covered by neither the IPT law nor the RGB law
// (refs/118 s9), so VP's mapping (dm_curve + dm_trims) steps back to libplacebo's own for that picture, visibly.

#include <VP-V/core/FallbackCatalog.h>

#include <string>

namespace DmContainer
{
	// containerName: "R12B" / "R12L" for those containers, else "" (the host names the capture's encoding).
	inline bool Unsupported(bool dolbyVisionTunnel, const std::string& containerName, const std::string& dvDisplayMapping)
	{
		return dolbyVisionTunnel && dvDisplayMapping != "original" && (containerName == "R12B" || containerName == "R12L");
	}

	// The DV mapping runs VP's way this frame: the setting asks for it and the container can carry it.
	inline bool VpMapping(bool dolbyVisionTunnel, const std::string& containerName, const std::string& dvDisplayMapping)
	{
		return dvDisplayMapping != "original" && !Unsupported(dolbyVisionTunnel, containerName, dvDisplayMapping);
	}

	inline FallbackCatalog::Decision ContainerRow(bool dolbyVisionTunnel, const std::string& containerName, const std::string& dvDisplayMapping)
	{
		const bool unsupported = Unsupported(dolbyVisionTunnel, containerName, dvDisplayMapping);
		return FallbackCatalog::DvDisplayMappingContainer(unsupported, unsupported ? containerName : std::string());
	}

	// calibration_lut_stage auto moved the LUT to VP's last step (resolved vp) and a LUT is present this frame.
	inline FallbackCatalog::Decision LutRow(const std::string& configuredStage, const std::string& resolvedStage, bool lutThisFrame)
	{
		return FallbackCatalog::CalibrationLutDisplayMapping(configuredStage == "auto" && resolvedStage == "vp" && lutThisFrame);
	}
}
