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
#include <VP-V/modules/repair/temporal_detail/v2/TdV2BlendMath.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Config.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Scheduler.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <sstream>
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

	// TD-U4 / Owner D4 statistics pooling selector:
	//   - BandShrink (default): 5x5 L / 3x3 M / (3x3 Ref or 1x1 RT) H + Empirical-Bayes shrinkage
	//   - TwoFrameSameRole: adds optional 2-frame same-role statistics-only pooling (NEVER pixels or weights)
	//   - Legacy1x1: disables band-dependent pooling and Empirical-Bayes shrinkage (ablation)
	enum class StatsPoolingMode : uint8_t
	{
		BandShrink = 0,
		TwoFrameSameRole = 1,
		Legacy1x1 = 2
	};

	inline const char* StatsPoolingModeToken(StatsPoolingMode m)
	{
		switch (m)
		{
		case StatsPoolingMode::BandShrink: return "band_shrink";
		case StatsPoolingMode::TwoFrameSameRole: return "two_frame_same_role";
		case StatsPoolingMode::Legacy1x1: return "legacy_1x1";
		}
		return "unknown";
	}

	struct TdV2RuntimeSettings
	{
		// Main VP-V > Repair > Main > Temporal detail keys
		Mode mode = Mode::Off;
		ResidualMode residualMode = ResidualMode::Cell;
		double limitKappa = kPixelBoundKappaDefault; // TD-U3 / Owner D3: strict default 1.0, range [1.0, 3.0]
		Preset preset = Preset::RealTime;            // Harness-only default "realtime"
		OverlayMode overlay = OverlayMode::Off;

		// Advanced / Developer modular ablation switches (VP-V > Repair > Advanced > Temporal detail: tuning)
		SolverMode solverMode = SolverMode::ActiveSetExact;
		QualitySignalMode qualitySignalMode = QualitySignalMode::AlignedSharpnessA2e;
		StatsPoolingMode statsPoolingMode = StatsPoolingMode::BandShrink;
		bool anchoredCarry = false;        // TD-U5 / D2: OFF by default until fault tests pass
		bool smoothWarp = true;            // 6.1 / L-1
		bool hardFbGate = true;            // 6.2 / M-3
		bool chromaReuseLuma = false;      // 6.6: independent (false) | reuse_luma (true)
		bool smoothGrainGuard = true;      // L-3
		bool subpixelDebias = false;       // SOTA-2
		bool edgeUpsample = false;         // SOTA-4
		bool neighborhoodClamp = false;    // SOTA-5
		bool regionTailoring = false;      // Stage A4-1: continuous smoothstep region modulation + Pass 3A edge-aware blending
		double bokehMatchDelta = 0.0;      // Stage A4-1: matched-focus bokeh gating tolerance (0.0 = off, else 0.125/0.25/0.375)
		bool regionDeband = false;         // Stage A4-1: direct L-band region-aware debanding + td.maps guidance
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
		if (k == "td_limit_kappa")
		{
			if (v.empty())
			{
				problem = "invalid td_limit_kappa value '" + value + "' (expected number in [1.0, 3.0])";
				return false;
			}
			char* endPtr = nullptr;
			const double parsed = std::strtod(v.c_str(), &endPtr);
			if (endPtr == nullptr || *endPtr != '\0' || !std::isfinite(parsed) ||
			    parsed < kPixelBoundKappaMin || parsed > kPixelBoundKappaMax)
			{
				problem = "invalid td_limit_kappa value '" + value + "' (expected number in [1.0, 3.0])";
				return false;
			}
			settings.limitKappa = parsed;
			return true;
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
		if (k == "td_solver")
		{
			if (v == "active_set") { settings.solverMode = SolverMode::ActiveSetExact; return true; }
			if (v == "legacy_clip") { settings.solverMode = SolverMode::LegacyClipRescale; return true; }
			problem = "invalid td_solver value '" + value + "' (expected active_set|legacy_clip)";
			return false;
		}
		if (k == "td_quality_signal")
		{
			if (v == "aligned_a2e") { settings.qualitySignalMode = QualitySignalMode::AlignedSharpnessA2e; return true; }
			if (v == "non_block_step") { settings.qualitySignalMode = QualitySignalMode::NonBlockPictureStep; return true; }
			if (v == "legacy_block_8px") { settings.qualitySignalMode = QualitySignalMode::LegacyBlockEdge8px; return true; }
			problem = "invalid td_quality_signal value '" + value + "' (expected aligned_a2e|non_block_step|legacy_block_8px)";
			return false;
		}
		if (k == "td_stats_pooling")
		{
			if (v == "band_shrink") { settings.statsPoolingMode = StatsPoolingMode::BandShrink; return true; }
			if (v == "two_frame_same_role") { settings.statsPoolingMode = StatsPoolingMode::TwoFrameSameRole; return true; }
			if (v == "legacy_1x1") { settings.statsPoolingMode = StatsPoolingMode::Legacy1x1; return true; }
			problem = "invalid td_stats_pooling value '" + value + "' (expected band_shrink|two_frame_same_role|legacy_1x1)";
			return false;
		}
		if (k == "td_chroma_weights")
		{
			if (v == "independent") { settings.chromaReuseLuma = false; return true; }
			if (v == "reuse_luma") { settings.chromaReuseLuma = true; return true; }
			problem = "invalid td_chroma_weights value '" + value + "' (expected independent|reuse_luma)";
			return false;
		}
		auto parseOnOff = [&](const char* keyName, bool& field) -> bool
		{
			if (v == "on") { field = true; return true; }
			if (v == "off") { field = false; return true; }
			problem = std::string("invalid ") + keyName + " value '" + value + "' (expected on|off)";
			return false;
		};
		if (k == "td_anchored_carry") return parseOnOff("td_anchored_carry", settings.anchoredCarry);
		if (k == "td_smooth_warp") return parseOnOff("td_smooth_warp", settings.smoothWarp);
		if (k == "td_hard_fb_gate") return parseOnOff("td_hard_fb_gate", settings.hardFbGate);
		if (k == "td_smooth_grain_guard") return parseOnOff("td_smooth_grain_guard", settings.smoothGrainGuard);
		if (k == "td_subpixel_debias") return parseOnOff("td_subpixel_debias", settings.subpixelDebias);
		if (k == "td_edge_upsample") return parseOnOff("td_edge_upsample", settings.edgeUpsample);
		if (k == "td_neighborhood_clamp") return parseOnOff("td_neighborhood_clamp", settings.neighborhoodClamp);
		if (k == "td_region_tailoring") return parseOnOff("td_region_tailoring", settings.regionTailoring);
		if (k == "td_region_deband") return parseOnOff("td_region_deband", settings.regionDeband);
		if (k == "td_bokeh_match_delta")
		{
			if (v == "off") { settings.bokehMatchDelta = 0.0; return true; }
			if (v == "0.125") { settings.bokehMatchDelta = kBokehMatchDeltaTight; return true; }
			if (v == "0.25") { settings.bokehMatchDelta = kBokehMatchDeltaNormal; return true; }
			if (v == "0.375") { settings.bokehMatchDelta = kBokehMatchDeltaWide; return true; }
			problem = "invalid td_bokeh_match_delta value '" + value + "' (expected off|0.125|0.25|0.375)";
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
		cfg.pixelBoundKappa = ClampPixelBoundKappa(s.limitKappa);
		cfg.solverMode = s.solverMode;
		cfg.qualitySignalMode = s.qualitySignalMode;
		cfg.enableA2eEmpiricalPrior = (s.qualitySignalMode == QualitySignalMode::AlignedSharpnessA2e);
		if (s.statsPoolingMode == StatsPoolingMode::Legacy1x1)
		{
			cfg.enableBandDependentPool = false;
			cfg.enableEmpiricalBayesShrinkage = false;
			cfg.enableTwoFrameSameRoleStatsPool = false;
			cfg.momentPoolRadiusCells = 0;
		}
		else
		{
			cfg.enableBandDependentPool = true;
			cfg.enableEmpiricalBayesShrinkage = true;
			cfg.enableTwoFrameSameRoleStatsPool = (s.statsPoolingMode == StatsPoolingMode::TwoFrameSameRole);
		}
		cfg.enableAnchoredCarry = s.anchoredCarry;
		cfg.enableSmoothWarpVectors = s.smoothWarp;
		cfg.enableHardFbOcclusionGate = s.hardFbGate;
		cfg.chromaReuseLumaWeights = s.chromaReuseLuma;
		cfg.enableSmoothLayer4Guard = s.smoothGrainGuard;
		cfg.enableSubpixelHAttenuationDebias = s.subpixelDebias;
		cfg.enableEdgeAwareWeightUpsample = s.edgeUpsample;
		cfg.enableNeighborhoodSoftClamp = s.neighborhoodClamp;
		cfg.enableRegionTailoring = s.regionTailoring;
		cfg.bokehMatchDelta = ClampBokehMatchDelta(s.bokehMatchDelta);
		cfg.enableRegionDeband = s.regionDeband;
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
		bool transformIdentified8x8 = false; // True only on identified 8x8 AVC/MPEG transform with real encoder prediction
		bool hasCodecModelPb = false;        // True only when real Field B CodecModel prediction P_B is available
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
		const double effKappa = ClampPixelBoundKappa(in.settings.limitKappa);
		std::ostringstream kappaOss;
		kappaOss << std::fixed << std::setprecision(1) << effKappa;

		const bool activeCellRegime = in.transformIdentified8x8 && in.hasCodecModelPb;

		std::string line = in.wiredToLiveRenderer
			? (std::string("Temporal detail: ") + ModeToken(in.settings.mode))
			: (std::string("Temporal detail v2: library ready (not wired to live renderer yet - connects at Wave 4-6 integration); requested=") + ModeToken(in.settings.mode));
		if (activeCellRegime)
		{
			line += ", limit: cell (AVC 8x8)";
			line += ", mode=" + std::string(ResidualModeToken(in.settings.residualMode));
		}
		else
		{
			line += ", limit: statistical (transform unknown), kappa=" + kappaOss.str();
			line += ", mode=" + std::string(ResidualModeToken(in.settings.residualMode)) + " (inactive: transform unknown)";
		}
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
