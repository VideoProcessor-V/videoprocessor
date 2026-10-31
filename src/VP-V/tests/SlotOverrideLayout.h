// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// T4 (2026-10-31): where the GPU harness writes a slot's Original-pathway overrides
// (EverySlotOffEqualsItsOriginalOverrides in RendererCaptureHarnessTests.cpp), shared with the CPU
// check SlotOverrideLayoutTests so the layout is proven to load before any GPU run.
//
// The harness's active profile is its input profile ([vprenderer.input.low_latency], where Run
// writes capture_path). A key the input-profile validator accepts goes there; any other key (for
// example downscale_light, a rendering-profile key) goes to [vprenderer.Default]: written into the
// input profile it makes the loader reject the whole file (found on the RTX run 2026-10-31: the
// scaling.output case rendered the fallback configuration, dithering on). [vpv] keys go to [vpv].
// capture_path is the run's capture path; entrance_gpu_drain is returned separately (one [vpv] line).

#include <RendererProfileConfig.h>
#include <VP-V/core/OriginalPathway.h>

#include <string>

namespace SlotOverrideLayout
{
	// The keys [vpv] owns: the vpvKeys array of RendererProfileConfig.h (SettingsFingerprintStructureTests
	// asserts the parsed array equals this list, so a new [vpv] key fails there until it is added here).
	inline bool IsVpvSectionKey(const std::string& key)
	{
		static const char* const keys[] = { "pipeline_original_pathway", "pipeline_order", "pipeline_slots_off",
			"calibration_lut_stage", "pipeline_entrance_once", "output_segments_merged", "frame_analysis_store",
			"entrance_gpu_drain", "hdr_inference" };
		for (const char* k : keys)
			if (key == k) return true;
		return false;
	}

	struct Layout
	{
		std::string inputKeys;      // lines for the input profile (before [vprenderer.Default])
		std::string defaultKeys;    // lines for [vprenderer.Default]
		std::string vpvKeys;        // lines for [vpv] (without entrance_gpu_drain)
		std::string capturePath;    // the run's capture path
		std::string drain;          // entrance_gpu_drain value for the [vpv] line
	};

	inline Layout ForSlot(const std::string& slot, const std::string& defaultCapturePath, const std::string& defaultDrain)
	{
		Layout l;
		l.capturePath = defaultCapturePath;
		l.drain = defaultDrain;
		for (const OriginalPathway::Override& o : OriginalPathway::Overrides())
		{
			if (slot != o.slot || !o.value[0]) continue;
			const std::string key = o.key, value = o.value;
			std::string expected;
			if (key == "entrance_gpu_drain") l.drain = value;
			else if (key == "capture_path") l.capturePath = value;
			else if (IsVpvSectionKey(key)) l.vpvKeys += key + ": " + value + "\n";
			else if (RendererProfileConfig::ValidateProfileSetting("input", key, value, expected)) l.inputKeys += key + ": " + value + "\n";
			else l.defaultKeys += key + ": " + value + "\n";
		}
		return l;
	}

	// The configuration text after Run's own "[general]" and input-profile lines: the input keys,
	// then `rendering` (the [vprenderer.Default] section header and its settings), the default keys,
	// and [vpv] with the drain and the [vpv] keys.
	inline std::string Extra(const Layout& l, const std::string& rendering)
	{
		return l.inputKeys + rendering + l.defaultKeys + "[vpv]\nentrance_gpu_drain: " + l.drain + "\n" + l.vpvKeys;
	}
}
