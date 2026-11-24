// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S7 (2026-11-23_TD-consolidated-plan.md s3.2 TD-S7, s4.3 Q8):
// Runtime settings parsing/validation, Original-pathway override, Owner Q8a dependency-plan helpers
// for Wave 4-6 renderer hookup (NOT wired to live renderer yet), TdProducts::Snapshot builder,
// and plain-English Ctrl+I / Config status formatting.
//
// HONEST STATUS: Temporal detail v2 is library-ready and NOT wired to the live renderer yet
// (connects at Wave 4-6 integration; TdProducts::kEngineRequestedToday remains false).
// Dependency-plan helpers here compute what Wave 4-6 will activate; toggling temporal_detail
// in Config today does NOT run TD in the live renderer or auto-enable the analysis lane yet.
//
// Keys:
//   - temporal_detail = off (default) | shadow | on
//   - td_mode         = cell (default) | strict
//   - td_preset       = realtime (default, harness-only) | reference
//   - td_overlay      = off (default) | weights | maps | focus | motion
//
// Header-only, C++14/17, no Windows headers.
#pragma once

#include <VP-V/modules/motion/estimation/LaneAheadPolicy.h>
#include <VP-V/modules/repair/temporal_detail/products/TdProductContracts.h>
#include <VP-V/modules/repair/temporal_detail/products/TdProductReads.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Config.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Scheduler.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace TdV2
{
	enum class OverlayMode : uint8_t
	{
		Off = 0,
		Weights = 1,
		Maps = 2,
		Focus = 3,
		Motion = 4
	};

	inline const char* OverlayModeToken(OverlayMode o)
	{
		switch (o)
		{
		case OverlayMode::Off: return "off";
		case OverlayMode::Weights: return "weights";
		case OverlayMode::Maps: return "maps";
		case OverlayMode::Focus: return "focus";
		case OverlayMode::Motion: return "motion";
		}
		return "unknown";
	}

	struct TdV2RuntimeSettings
	{
		Mode mode = Mode::Off;
		ResidualMode residualMode = ResidualMode::Cell;
		Preset preset = Preset::RealTime;       // Harness-only default "realtime"
		OverlayMode overlay = OverlayMode::Off;
	};

	namespace Detail
	{
		inline std::string NormalizeSettingToken(const std::string& raw)
		{
			size_t b = 0;
			while (b < raw.size() && (raw[b] == ' ' || raw[b] == '\t' || raw[b] == '\r' || raw[b] == '\n'))
				++b;
			size_t e = raw.size();
			while (e > b && (raw[e - 1] == ' ' || raw[e - 1] == '\t' || raw[e - 1] == '\r' || raw[e - 1] == '\n'))
				--e;
			std::string out;
			out.reserve(e - b);
			for (size_t i = b; i < e; ++i)
			{
				const char c = raw[i];
				out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
			}
			return out;
		}
	}

	// Strict settings parser: validates key & value, updates `settings` ONLY when valid, and writes
	// an explicit diagnostic into `problem` on rejection (Rule 4: never silently mutates on bad input).
	inline bool ParseTdV2Settings(
		const std::string& key,
		const std::string& value,
		TdV2RuntimeSettings& settings,
		std::string& problem)
	{
		const std::string k = Detail::NormalizeSettingToken(key);
		const std::string v = Detail::NormalizeSettingToken(value);
		problem.clear();

		if (k == "temporal_detail")
		{
			if (v == "off") { settings.mode = Mode::Off; return true; }
			if (v == "shadow") { settings.mode = Mode::Shadow; return true; }
			if (v == "on") { settings.mode = Mode::On; return true; }
			problem = "invalid temporal_detail value '" + value + "' (expected off|shadow|on)";
			return false;
		}
		if (k == "td_mode")
		{
			if (v == "cell") { settings.residualMode = ResidualMode::Cell; return true; }
			if (v == "strict") { settings.residualMode = ResidualMode::Strict; return true; }
			problem = "invalid td_mode value '" + value + "' (expected cell|strict)";
			return false;
		}
		if (k == "td_preset")
		{
			if (v == "realtime") { settings.preset = Preset::RealTime; return true; }
			if (v == "reference") { settings.preset = Preset::Reference; return true; }
			problem = "invalid td_preset value '" + value + "' (expected realtime|reference)";
			return false;
		}
		if (k == "td_overlay")
		{
			if (v == "off") { settings.overlay = OverlayMode::Off; return true; }
			if (v == "weights") { settings.overlay = OverlayMode::Weights; return true; }
			if (v == "maps") { settings.overlay = OverlayMode::Maps; return true; }
			if (v == "focus") { settings.overlay = OverlayMode::Focus; return true; }
			if (v == "motion") { settings.overlay = OverlayMode::Motion; return true; }
			problem = "invalid td_overlay value '" + value + "' (expected off|weights|maps|focus|motion)";
			return false;
		}

		problem = "unknown temporal detail key '" + key + "'";
		return false;
	}

	// Build engine Config from runtime settings. Original pathway strictly forces Mode::Off (Original == Bill bit-exact).
	inline Config BuildTdV2Config(const TdV2RuntimeSettings& s, bool originalPathway)
	{
		Config cfg = MakePreset(s.preset, s.mode);
		cfg.residualMode = s.residualMode;
		if (originalPathway)
			ApplyOriginalPathwayOverride(cfg);
		return cfg;
	}

	// Owner Q8a dependency-plan helper for Wave 4-6 renderer hookup (NOT wired to live renderer yet):
	// computes which dependencies Wave 4-6 will activate when temporal_detail != off; Original pathway or off leaves both disabled.
	struct TdDependencyActivation
	{
		bool enableAnalysisLane = false;
		bool enableMotionCameraGroup = false;
		std::string visibleNote;
	};

	inline TdDependencyActivation ResolveTdDependencies(
		const TdV2RuntimeSettings& s,
		bool originalPathway)
	{
		TdDependencyActivation dep;
		if (originalPathway)
		{
			dep.visibleNote = "Temporal detail v2: off (Original pathway)";
			return dep;
		}
		if (s.mode == Mode::Off)
		{
			dep.visibleNote = "Temporal detail v2: off (library ready, not wired to live renderer yet)";
			return dep;
		}
		dep.enableAnalysisLane = true;
		dep.enableMotionCameraGroup = true;
		dep.visibleNote = std::string("Temporal detail v2: library ready (not wired to live renderer yet - connects at Wave 4-6 integration); ") +
			"Wave 4-6 plan for " + ModeToken(s.mode) + ": analysis lane + motion/camera group";
		return dep;
	}

	// ---- TdProducts::Snapshot Builder (Wave 4-6 renderer hookup; kEngineRequestedToday remains false today) ----

	struct LiveTdProductInputs
	{
		TdV2RuntimeSettings settings;
		bool originalPathway = false;
		int64_t timeUs = 0;
		int64_t maxAgeUs = 0;
		bool mapsValid = false;
		bool deltaValid = false;
		bool trailValid = false;
		bool blocksValid = false;
		bool memoryValid = false;
		bool anchorPassValid = false;
		bool anchorRefsValid = false;
		bool codingEvidenceValid = false;
		bool qualityPatternValid = false;
		bool qualityPatternCausalValid = false;
		bool refinedFieldValid = false;
		bool lateCarried = false;
		bool lateExpired = false;
	};

	struct LiveTdProductSnapshot
	{
		bool engineRequested = false;
		uint32_t unit = 0;
		bool mapsValid = false;
		bool deltaValid = false;
		bool trailValid = false;
		std::string statusText;
		TdProducts::Snapshot snapshot{};

		// Returns TdProducts::PlaceholderSnapshot() when OFF or Original, or &snapshot when live.
		const TdProducts::Snapshot* View() const
		{
			return engineRequested ? &snapshot : TdProducts::PlaceholderSnapshot();
		}
	};

	inline LiveTdProductSnapshot BuildLiveTdProductSnapshot(const LiveTdProductInputs& in)
	{
		LiveTdProductSnapshot out;
		if (in.originalPathway || in.settings.mode == Mode::Off)
		{
			out.engineRequested = false;
			out.unit = 0;
			out.snapshot = *TdProducts::PlaceholderSnapshot();
			out.statusText = in.originalPathway
				? "temporal detail: off (Original pathway)"
				: "temporal detail: off";
			return out;
		}

		out.engineRequested = true;
		out.unit = TdProducts::kUnitVersion;
		out.mapsValid = in.mapsValid;
		out.deltaValid = in.deltaValid;
		out.trailValid = in.trailValid;

		auto publish = [&](VpSkeleton::ProductId id, bool valid, uint32_t unitVer)
		{
			const int idx = TdProducts::IndexOf(id);
			if (idx >= 0 && idx < TdProducts::kCount && valid)
			{
				out.snapshot.entry[idx].published = true;
				out.snapshot.entry[idx].unitVersion = unitVer;
				out.snapshot.entry[idx].timeUs = in.timeUs;
				out.snapshot.entry[idx].maxAgeUs = in.maxAgeUs;
			}
		};

		// Populate each product using its exact contract unitVersion from TdProductContracts.h.
		publish(VpSkeleton::kTdMaps, in.mapsValid, TdProducts::kMapsViewUnit);
		publish(VpSkeleton::kTdDelta, in.deltaValid, TdProducts::kUnitVersion);
		publish(VpSkeleton::kTdTrailSummary, in.trailValid, TdProducts::kUnitVersion);
		publish(VpSkeleton::kTdBlocks, in.blocksValid, TdProducts::kUnitVersion);
		publish(VpSkeleton::kTdMemory, in.memoryValid, TdProducts::kUnitVersion);
		publish(VpSkeleton::kTdAnchorPass, in.anchorPassValid, TdProducts::kUnitVersion);
		publish(VpSkeleton::kTdAnchorRefs, in.anchorRefsValid, TdProducts::kUnitVersion);
		publish(VpSkeleton::kCodingEvidence, in.codingEvidenceValid, TdProducts::kCodingEvidenceUnit);
		publish(VpSkeleton::kQualityPattern, in.qualityPatternValid, TdProducts::kQualityPatternUnit);
		publish(VpSkeleton::kQualityPatternCausal, in.qualityPatternCausalValid, TdProducts::kQualityPatternUnit);
		publish(VpSkeleton::kRefinedField, in.refinedFieldValid, TdProducts::kFieldRefineUnit);

		if (in.lateExpired)
			out.statusText = MetaOf(StandInKind::LateCarryExpired).statusText;
		else if (in.lateCarried)
			out.statusText = MetaOf(StandInKind::LateCarryActive).statusText;
		else
			out.statusText = std::string("Temporal detail v2: library ready (not wired to live renderer yet - connects at Wave 4-6 integration); snapshot=") + ModeToken(in.settings.mode);

		return out;
	}

	// ---- Plain-English Ctrl+I / Config Status Formatter ----

	struct TdCtrlIStatusInput
	{
		TdV2RuntimeSettings settings;
		bool originalPathway = false;
		bool wiredToLiveRenderer = false; // Honest default: library ready, not wired to live renderer yet (Wave 4-6 hookup)
		TdTier tier = TdTier::Full;
		int usableLeadE = VpMotion::LaneAhead::kDepthTarget;
		bool lateCarried = false;
		bool lateExpired = false;
		int lateStreak = 0;
		const StandInRegistry* standIns = nullptr;
	};

	inline std::string FormatTdCtrlIStatus(const TdCtrlIStatusInput& in)
	{
		if (in.originalPathway)
			return "Temporal detail v2: off (Original pathway)";
		if (in.settings.mode == Mode::Off)
			return in.wiredToLiveRenderer
				? "Temporal detail: off"
				: "Temporal detail v2: off (library ready, not wired to live renderer yet - connects at Wave 4-6 integration)";

		const int ceiling = static_cast<int>(VpSkeleton::kAheadCeiling);
		const int target = VpMotion::LaneAhead::kDepthTarget;
		const int clampedE = (std::max)(0, (std::min)(ceiling, in.usableLeadE));

		std::string line = in.wiredToLiveRenderer
			? (std::string("Temporal detail: ") + ModeToken(in.settings.mode))
			: (std::string("Temporal detail v2: library ready (not wired to live renderer yet - connects at Wave 4-6 integration); requested=") + ModeToken(in.settings.mode));
		line += ", mode=" + std::string(ResidualModeToken(in.settings.residualMode));
		if (in.settings.overlay != OverlayMode::Off)
			line += ", overlay=" + std::string(OverlayModeToken(in.settings.overlay));
		line += ", tier=" + std::string(TdTierName(in.tier));
		line += ", lead E=" + std::to_string(clampedE) + "/" + std::to_string(target);

		if (in.lateExpired)
		{
			line += ", " + std::string(MetaOf(StandInKind::LateCarryExpired).statusText);
		}
		else if (in.lateCarried)
		{
			line += ", " + std::string(MetaOf(StandInKind::LateCarryActive).statusText);
			if (in.lateStreak > 0)
				line += " (" + std::to_string(in.lateStreak) + "/" + std::to_string(kMaxLateCarryFrames) + ")";
		}
		else
		{
			line += ", timing=on-time";
		}

		if (in.standIns != nullptr && in.standIns->AnyActive())
		{
			line += " | stand-ins: ";
			line += in.standIns->FormatStatusSummary();
		}
		return line;
	}
}
