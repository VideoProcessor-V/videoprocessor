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

	// Hard forward/backward occlusion gate threshold in source pixels (refs/111 s6 / Fable M-3 / Plan 6.2):
	// vector disagreement > 1.0 px hard-zeros blend weight (0.0); continuous confidence tapers below 1.0 px.
	constexpr double kHardFbOcclusionPx = 1.0;

	// Quarter-pel anchor trail drift bar in source pixels (P3r tracking precision floor = 1/4 px):
	// catches sustained >= 0.05 px/frame composed-vector drift cleanly within one 8-frame GOP span.
	constexpr double kAnchorDriftBarPx = 0.25;

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

	// Canonical reference slot role across luma/chroma CPU & GPU blend engines (CHR-HIGH-3 / UPG-HIGH-6).
	enum class RefRole : uint8_t
	{
		PrevPeak = 0,
		NextPeak = 1,
		PrevPeak2 = 2,
		Carry = 3
	};

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

	// Picture-only frame-role & own-error signal estimator mode (TD-U1 / B-2 / Owner D1):
	//   - AlignedSharpnessA2e (default): motion-aligned relative sharpness r_t + empirical A2e own-error prior U_j(class)
	//   - NonBlockPictureStep: grid-free picture-only staircase-jump + fine-band dead-zone coding-step estimator (no 8-px edges)
	//   - LegacyBlockEdge8px: original 8-px block-boundary excess gradient (optional AVC/MPEG-2 bonus cue / diagnostic)
	enum class QualitySignalMode : uint8_t
	{
		AlignedSharpnessA2e = 0,
		NonBlockPictureStep = 1,
		LegacyBlockEdge8px = 2
	};

	inline const char* QualitySignalModeToken(QualitySignalMode q)
	{
		switch (q)
		{
		case QualitySignalMode::AlignedSharpnessA2e: return "aligned-sharpness-a2e";
		case QualitySignalMode::NonBlockPictureStep: return "non-block-picture-step";
		case QualitySignalMode::LegacyBlockEdge8px: return "legacy-block-edge-8px";
		}
		return "unknown";
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
			{ "SC-01", "H-band moment pooling radius: 3x3 cells (Reference) vs 1x1 cell (RealTime); L stays 5x5 and M stays 3x3 when band-dependent pooling is enabled", false, true },
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
		QualitySignalMode qualitySignalMode = QualitySignalMode::AlignedSharpnessA2e;
		bool enableA2eEmpiricalPrior = true;
		Preset preset = Preset::Reference;
		BokehMode bokehMode = BokehMode::Untouched;

		// Honest transform-unknown output limit multiplier kappa in [1.0, 3.0] (TD-U3 / Owner D3):
		// strict default 1.0: |Delta_j(p)| <= kappa * sqrt(U_j(block)) and |sum_j Delta_j(p)| <= kappa * sqrt(sum_j U_j(block)).
		double pixelBoundKappa = 1.0;
		bool enableJointCrossBandBound = true;
		// Per-cell AQ dQP step widening (M-6): OFF by default until a live HDMI AQ producer exists;
		// PQ perceptual weighting may only tighten (<= 1.0), never widen beyond 1.0.
		bool enableAqStepScaling = false;

		// Source code depth in bits (10 = standard 10-bit pipeline; scaled via DepthScale(sourceBits) = 2^(bits - 10)).
		int sourceBits = 10;

		// Canonical TD cell edge (16 px) and upstream P10/P10g block edge (mapped dynamically per P10-D6, never hardcoded 16 vs 32).
		int cellPx = kCanonicalBlockPx;
		int grainCellPx = kCanonicalBlockPx;

		// Neighbour depths matching ComposedVectorPlan.h / AheadCeiling.h contracts.
		int maxBackRefs = kContractBackRefs;
		int maxAheadRefs = kContractAheadRefs;
		int aheadCeiling = kContractAheadCeiling;

		// Dual-source fusion & carry controls (TD-U5 / Owner D2):
		// - enableCarryForward (legacy chained carry): DEFAULT OFF per Owner Decision #3 & D2.
		// - enableAnchoredCarry (single-resample anchored carry from last confirmed peak with held-out structure check
		//   and real kDrift audit): DEFAULT OFF per Owner D2 until injected-fault tests pass.
		bool enableCarryForward = false;
		bool enableAnchoredCarry = false;
		int maxAnchorAgeFrames = kContractBackRefs; // Bounded by composed-vector depth (8 back, ComposedVectorPlan.h)
		bool enableLongAnchor = false;

		// Modular low-cost quality switches (Plan Phase 6 + Fable audit M-3, M-9, L-1, L-3):
		bool enableSmoothWarpVectors = true;   // 6.1 / L-1: bilinearly interpolate 16-px cell vectors in warp (vs nearest-cell)
		bool enableHardFbOcclusionGate = true; // 6.2 / M-3: hard > 1.0 px forward/backward error -> weight 0, continuous taper below
		bool chromaReuseLumaWeights = false;   // 6.6: optional toggle letting chroma reuse luma weights (laptop budget comparison)
		bool enableSmoothLayer4Guard = true;   // L-3: bilinearly interpolate Layer-4 grain-guard add-back fraction across cells
		bool enableGpuCounterSummary = true;   // M-9: compact 2-texel (32-byte) live GPU counter summary readback in zero-copy mode

		// Reference vs RealTime switches aligned to ShortcutLedger().
		int momentPoolRadiusCells = 1;  // H-band (or legacy uniform) radius: 1 => 3x3 cells (Reference), 0 => 1x1 cell (RealTime SC-01)
		int cellProjIterations = 2;     // <= 2 POCS iterations per refs/111 s5

		// TD-U4 (H-3 / Owner D4): Steadier statistics & flicker controls (all cleanly toggleable for ablation).
		// - enableBandDependentPool (default true): 5x5 cells (r=2) for L, 3x3 (r=1) for M, momentPoolRadiusCells for H (SC-01 applies to H only).
		// - enableEmpiricalBayesShrinkage (default true): shrinks raw c_IV toward frame prior U_j(class) using se^2 and tau^2.
		// - enableTwoFrameSameRoleStatsPool (DEFAULT OFF per Owner D4(b)): pools ONLY cell statistics (C, c_raw - NEVER pixels or weights)
		//   across at most 2 recent frames of the same role; resets on cut/fade/size change.
		bool enableBandDependentPool = true;
		bool enableEmpiricalBayesShrinkage = true;
		bool enableTwoFrameSameRoleStatsPool = false;

		int PoolRadiusForBand(int band) const
		{
			const int baseR = momentPoolRadiusCells > 0 ? momentPoolRadiusCells : 0;
			if (!enableBandDependentPool)
				return baseR;
			if (band == 0)
				return 2; // 5x5 cells for coarse band L (~163 effective independent samples)
			if (band == 1)
				return 1; // 3x3 cells for mid band M (~367 effective independent samples)
			return baseR; // 3x3 (Reference) or 1x1 (RealTime SC-01) for fine band H
		}

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
