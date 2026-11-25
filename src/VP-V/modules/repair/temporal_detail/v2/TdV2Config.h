// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S0 (2026-11-23_TD-consolidated-plan.md s3.2):
// Canonical configuration knobs, presets (Reference vs RealTime with explicit SC-* shortcut ledger),
// depth scaling, and dynamic P10/P10g block-grid mapping.
//
// Defaults:
//   - mode = Mode::Off (OFF by default; Original pathway forces Mode::Off)
//   - residualMode = ResidualMode::Cell (STRICT selectable per Owner Q1)
//   - estimator = VarianceEstimator::A3_ConservativeMin (A1/A2/A3 selectable per refs/111 s3)
//   - bokehMode = BokehMode::Untouched (L-band bokeh study mode selectable per Owner Q4)
//
// Every default parameter is derived from signal physics, codec geometry, or existing VP-V contracts
// (Rule 1: zero arbitrary constants). Header-only, C++14/17, no Windows headers.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace TdV2
{
	// Schema version of the TD v2 CPU reference & contract headers.
	constexpr uint32_t kTdV2Version = 1;

	// Canonical TD block edge in source pixels (matches TdMapsView U19 and QualityPattern::kBlockGridCellEdge = 16;
	// standard codec macroblock / HEVC CTU sub-block size).
	constexpr int kCanonicalBlockPx = 16;

	// Neighbour-set depth contract from ComposedVectorPlan.h (8 back + 4 ahead) and AheadCeiling.h (cap 6).
	constexpr int kContractBackRefs = 8;
	constexpr int kContractAheadRefs = 4;
	constexpr int kContractAheadCeiling = 6;

	// Standard cinema 23.976 Hz frame rate (24000 / 1001) used only as default rate when caller supplies no measured Hz.
	constexpr double kDefaultSourceHz = 24000.0 / 1001.0;

	// Top-level operating mode. Default is strictly Off.
	enum class Mode : uint8_t
	{
		Off = 0,
		Shadow = 1,   // Computes maps & diagnostics without modifying output pixels
		On = 2
	};

	inline const char* ModeToken(Mode m)
	{
		switch (m)
		{
		case Mode::Off: return "off";
		case Mode::Shadow: return "shadow";
		case Mode::On: return "on";
		}
		return "unknown";
	}

	// Quantization-cell consistency mode (refs/111 s5; Owner Q1: both built, Cell default, Strict selectable).
	enum class ResidualMode : uint8_t
	{
		Cell = 0,     // Sent coefficients may move inside their own rounding cell I_k; zeroed inside [-theta, +theta]
		Strict = 1    // Sent coefficients restored exact; only zeroed coefficients move inside [-theta, +theta]
	};

	inline const char* ResidualModeToken(ResidualMode m)
	{
		return m == ResidualMode::Strict ? "strict" : "cell";
	}

	// Blind error-covariance estimator for c_j (refs/111 s3.2-3.4).
	enum class VarianceEstimator : uint8_t
	{
		A1_ThreeCorneredHat = 0,  // Grain-corrected instrumental variable E[d_p d_n] - kappa_g * N_g
		A2_RoundingCells = 1,     // Codec rounding prior from dead-zone step variance V_0, V_1
		A3_ConservativeMin = 2    // min(A1, A2) — primary default
	};

	inline const char* EstimatorToken(VarianceEstimator e)
	{
		switch (e)
		{
		case VarianceEstimator::A1_ThreeCorneredHat: return "A1";
		case VarianceEstimator::A2_RoundingCells: return "A2";
		case VarianceEstimator::A3_ConservativeMin: return "A3";
		}
		return "?";
	}

	// Constrained convex blend solver mode (TD-U2 / H-2):
	//   - ActiveSetExact (default): solves exact QP min w'Cw - 2 w'c s.t. w_k >= 0, sum w_k <= 1 by subset enumeration
	//   - LegacyClipRescale: original refs/111 s3.5 unconstrained solve + clip-then-rescale (retained strictly for ablation)
	enum class SolverMode : uint8_t
	{
		ActiveSetExact = 0,
		LegacyClipRescale = 1
	};

	inline const char* SolverModeToken(SolverMode s)
	{
		return s == SolverMode::LegacyClipRescale ? "legacy-clip-rescale" : "active-set-exact";
	}

	// Single-codebase preset selector (99 s2B: Reference vs RealTime, every shortcut explicitly named SC-*).
	enum class Preset : uint8_t
	{
		Reference = 0,
		RealTime = 1
	};

	inline const char* PresetToken(Preset p)
	{
		return p == Preset::RealTime ? "realtime" : "reference";
	}

	// Focus Rule bokeh policy (Owner Q4: M/H are ALWAYS off in defocused regions; L-band bokeh cleanup is study-selectable).
	enum class BokehMode : uint8_t
	{
		Untouched = 0,        // Defocused regions untouched on all bands (L, M, H)
		LowBandCellOnly = 1   // Matched-focus bokeh allows L-band quantization-cell repair only; M/H remain 0
	};

	inline const char* BokehModeToken(BokehMode b)
	{
		return b == BokehMode::LowBandCellOnly ? "l-band-cell-only" : "untouched";
	}

	// Named RealTime shortcut ledger entry (99 s2B: every difference between Reference and RealTime is named & audited).
	struct ShortcutEntry
	{
		const char* id;
		const char* description;
		bool activeInReference;
		bool activeInRealTime;
	};

	inline const ShortcutEntry* ShortcutLedger(size_t& count)
	{
		static const ShortcutEntry kLedger[] = {
			{ "SC-01", "Moment pooling radius: 3x3 cells (Reference) vs 1x1 cell (RealTime optional)", false, true },
			{ "SC-02", "Long-reach I-anchor y_4: enabled when drift-audited (Reference) vs local peaks+carry only (RealTime)", false, true },
			{ "SC-03", "Cell projection iterations: 2 POCS steps (Reference) vs 1 POCS step (RealTime)", false, true },
			{ "SC-04", "Weight upsampling: optional clean-luma-guided edge-aware bilinear (enableEdgeAwareWeightUpsample) vs spatial bilinear with per-corner 3-sigma Huber taper (default)", false, true },
			{ "SC-05", "GPU band split: full-res FP32 L/D3/M/H (Reference) vs half-res FP32 (L, D3) band split with full-res FP32 (M, H) (RealTime)", false, true }
		};
		count = sizeof(kLedger) / sizeof(kLedger[0]);
		return kLedger;
	}

	struct Config
	{
		Mode mode = Mode::Off;
		ResidualMode residualMode = ResidualMode::Cell;
		VarianceEstimator estimator = VarianceEstimator::A3_ConservativeMin;
		SolverMode solverMode = SolverMode::ActiveSetExact;
		Preset preset = Preset::Reference;
		BokehMode bokehMode = BokehMode::Untouched;

		// Source code depth in bits (10 = standard 10-bit pipeline; scaled via DepthScale(sourceBits) = 2^(bits - 10)).
		int sourceBits = 10;

		// Canonical TD cell edge (16 px) and upstream P10/P10g block edge (mapped dynamically per P10-D6, never hardcoded 16 vs 32).
		int cellPx = kCanonicalBlockPx;
		int grainCellPx = kCanonicalBlockPx;

		// Neighbour depths matching ComposedVectorPlan.h / AheadCeiling.h contracts.
		int maxBackRefs = kContractBackRefs;
		int maxAheadRefs = kContractAheadRefs;
		int aheadCeiling = kContractAheadCeiling;

		// Dual-source fusion controls (ProVQE-validated: recurrent carry + prev/next peaks).
		bool enableCarryForward = true;
		bool enableLongAnchor = false;

		// Reference vs RealTime switches aligned to ShortcutLedger().
		int momentPoolRadiusCells = 1;  // 1 => 3x3 cells (48 px at 16 px cells, refs/111 s3.5); 0 => 1x1 cell
		int cellProjIterations = 2;     // <= 2 POCS iterations per refs/111 s5

		// Per-pixel 3-sigma per-corner Huber outlier taper psi(r) = min(1, 9/r) during spatial weight recomposition.
		// Default true; setting false disables per-corner Huber taper on the exact same engine (negative control).
		bool enablePerCornerHuber = true;

		// Sub-pixel bicubic H-band attenuation debiasing in EstimateC_A1 (SOTA #2; default false for baseline parity).
		bool enableSubpixelHAttenuationDebias = false;

		// Clean-luma-guided edge-aware weight upsampling in Pass 3A (SOTA #4 / SC-04; default false for baseline parity).
		bool enableEdgeAwareWeightUpsample = false;

		// Grain-free 3x3 LDS neighborhood 3-sigma soft clamp on warped reference M/H bands (SOTA #5; default false).
		bool enableNeighborhoodSoftClamp = false;

		// Measured source rate in Hz (used to express any temporal horizon in seconds, never hardcoded frame counts).
		double sourceHz = kDefaultSourceHz;

		bool IsEnabled() const { return mode != Mode::Off; }
		bool ModifiesPixels() const { return mode == Mode::On; }
	};

	// Code-domain scale relative to 10-bit reference (2^(bits - 10)); energy scale is DepthScale^2.
	// Identical law to GrainSplit::DepthScale without creating a header coupling.
	inline double DepthScale(int sourceBits)
	{
		return std::ldexp(1.0, sourceBits - 10);
	}

	// Maximum code value for unsigned integer code representation at `sourceBits` (e.g. 1023 at 10-bit, 4095 at 12-bit).
	inline double PeakCodeForBits(int sourceBits)
	{
		if (sourceBits <= 0 || sourceBits > 24)
			return 0.0;
		return std::ldexp(1.0, sourceBits) - 1.0;
	}

	// Dynamic P10-D6 block-grid coordinate mapping: maps a canonical TD cell index (at cellPx) to its enclosing
	// P10/P10g cell index (at grainCellPx), supporting arbitrary power-of-two ratios (e.g., 16 -> 32 at 4K).
	inline int MapCellToGrainGrid(int tdCellIndex, int cellPx, int grainCellPx)
	{
		if (tdCellIndex < 0 || cellPx <= 0 || grainCellPx <= 0)
			return 0;
		return (tdCellIndex * cellPx) / grainCellPx;
	}

	// Build canonical Reference or RealTime preset config (mode stays Off by default until caller opts in).
	inline Config MakePreset(Preset preset, Mode mode = Mode::Off)
	{
		Config cfg;
		cfg.mode = mode;
		cfg.preset = preset;
		if (preset == Preset::Reference)
		{
			cfg.momentPoolRadiusCells = 1;  // 3x3 pooling (SC-01 off)
			cfg.enableLongAnchor = true;    // SC-02 off (still gated by drift stand-in availability)
			cfg.cellProjIterations = 2;     // SC-03 off
		}
		else
		{
			cfg.momentPoolRadiusCells = 0;  // 1x1 pooling (SC-01 active)
			cfg.enableLongAnchor = false;   // SC-02 active
			cfg.cellProjIterations = 1;     // SC-03 active
		}
		return cfg;
	}

	// Original pathway law: forces TD strictly Off so Original == Bill bit-exact.
	inline void ApplyOriginalPathwayOverride(Config& cfg)
	{
		cfg.mode = Mode::Off;
	}
}
