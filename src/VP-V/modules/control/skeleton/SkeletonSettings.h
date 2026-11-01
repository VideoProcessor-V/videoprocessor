// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Section [vpv.skeleton] of VideoProcessor.cfg: the two switches that activate the skeleton's analysis
// lane (SkeletonModule) and the camera link's reporting (CameraExtension) through the hub.
//
//   analysis_lane                off | shadow | on      (default off)
//   analysis_lane_chunk_us       0..4000000             (default 0 = derived from the frame period)
//   analysis_lane_deterministic  auto | on | off        (default auto)
//   analysis_lane_test_wait_ms   0..4000000             (default 0 = none)
//   camera_link                  off | report           (default off)
//
// The lane keys are VpSkeleton::LaneSettings::Parse (one parser). An invalid value keeps the default and
// is listed in `laneProblems` / `cameraProblems` (shown in the owning module Fallbacks status, never log-only). The renderer's
// config validator accepts any [vpv.*] section without key checks, so no schema entry exists.
// Original pathway ([vpv] pipeline_original_pathway = on, or SettingsApplied with code != 0): every key
// behaves as off. Header-only, C++14.

#include <ConfigFile.h>
#include <VP-V/core/AnalysisLane.h>

#include <functional>
#include <string>
#include <vector>

namespace Vpv
{
	namespace Skeleton
	{
		constexpr const char* kSection = "vpv.skeleton";

		enum class CameraLinkMode { Off, Report };

		struct Settings
		{
			VpSkeleton::LaneSettings lane;                 // mode Off by default
			CameraLinkMode camera = CameraLinkMode::Off;
			bool originalPathway = false;                  // [vpv] pipeline_original_pathway = on
			std::vector<std::string> laneProblems;         // invalid analysis_lane* values (kept at the default)
			std::vector<std::string> cameraProblems;       // invalid camera_link value (kept off)

			bool LaneWanted() const { return !originalPathway && lane.mode != VpSkeleton::LaneSettings::Mode::Off; }
			bool CameraWanted() const { return !originalPathway && camera == CameraLinkMode::Report; }
		};

		// A loader of the configuration file (the working directory's VideoProcessor.cfg by default).
		// Injectable so tests need no file. Returns false when no file could be read (all keys default).
		using ConfigLoader = std::function<bool(ConfigFile&)>;

		// The renderer's selected configuration path (SetRendererConfigurationPath, resolved by
		// Load(RENDERER_FILENAME)) when the host set one, else the working directory's VideoProcessor.cfg
		// (the application configuration the renderer then uses).
		inline bool DefaultLoader(ConfigFile& config)
		{
			if (!ConfigFile::GetRendererConfigurationPath().empty())
				return config.Load(ConfigFile::RENDERER_FILENAME);
			return config.Load();
		}

		inline Settings Parse(const ConfigFile& config)
		{
			Settings s;
			static const char* const kLaneKeys[] = { "analysis_lane", "analysis_lane_chunk_us", "analysis_lane_deterministic",
				"analysis_lane_test_wait_ms" };
			for (const char* key : kLaneKeys)
			{
				std::string raw;
				if (!config.TryGetString(kSection, key, raw))
					continue;
				const std::string value = ConfigFile::NormalizeName(raw);
				if (!s.lane.Parse(key, value.c_str()))
					s.laneProblems.push_back(std::string(key) + " '" + raw + "' is not valid; kept at its default");
			}
			std::string camera;
			if (config.TryGetString(kSection, "camera_link", camera))
			{
				const std::string value = ConfigFile::NormalizeName(camera);
				if (value == "off")
					s.camera = CameraLinkMode::Off;
				else if (value == "report")
					s.camera = CameraLinkMode::Report;
				else
					s.cameraProblems.push_back("camera_link '" + camera + "' is not valid (off|report); kept off");
			}
			std::string pathway;
			if (config.TryGetString("vpv", "pipeline_original_pathway", pathway) ||
				config.TryGetString("vprenderer", "pipeline_original_pathway", pathway))
				s.originalPathway = ConfigFile::NormalizeName(pathway) == "on";
			return s;
		}

		// Reads through `loader` (null = the working directory's file). An unreadable file gives defaults
		// (everything off); the caller has nothing to show then because nothing was asked for.
		inline Settings Load(const ConfigLoader& loader = ConfigLoader())
		{
			ConfigFile config;
			const bool ok = loader ? loader(config) : DefaultLoader(config);
			if (!ok)
				return Settings();
			return Parse(config);
		}
	}
}
