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

#include <algorithm>
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

	// Stage A4-1 matched-focus bokeh tolerance constants in normalized [0, 1] focus space (Rule 1: elementary dyadic fractions):
	// upper bound 0.5 = half of [0, 1] focus span so a full sharp (1.0) vs defocused (0.0) rack-focus jump can never match.
	constexpr double kBokehMatchDeltaTight = 0.125;   // 1/8 of [0, 1] focus span
	constexpr double kBokehMatchDeltaNormal = 0.25;   // 1/4 of [0, 1] focus span
	constexpr double kBokehMatchDeltaWide = 0.375;    // 3/8 of [0, 1] focus span
	constexpr double kBokehMatchDeltaMax = 0.5;       // 1/2 of [0, 1] focus span

	// Packed reason bit 3 in U19 TdMapsView::kBandMask ("active-band mask + reason"):
	// bits 0..2 remain L/M/H active bands (0..7); bit 3 (8) marks direct L-band region-aware debanding active (Stage A4-1).
	constexpr unsigned kTdMapsBandMaskRegionDebandBit = 8u;

	// Pass 3A derived RealTime budget-protection constants (Rule 1: derived from UHD geometry, 50/60p frame budget, and dyadic fractions):
	// - UHD 4K pixel count (3840 x 2160) & high-frame-rate cutoff (50.0 Hz => <= 20 ms frame budget vs 24/30p 33-41.7 ms).
	constexpr int kAuto4k60MinPixels = 3840 * 2160;
	constexpr double kAuto4k60MinHz = 50.0;
	// - Coverage-adaptive K=2->1 hysteresis bounds in [0, 1] active-tile coverage (elementary dyadic 3/4 enter, 5/8 exit => 1/8 deadband).
	constexpr double kCoverageAdaptiveEnterFraction = 0.75;
	constexpr double kCoverageAdaptiveExitFraction = 0.625;
	// - Maximum source bit depth eligible for 16-bit normalized integer RealTime band storage (SC-06: 16 bits preserves >= 3.4 sub-LSB bits at 12-bit!).
	constexpr int kNorm16BandsMaxSourceBits = 12;

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
			{ "SC-05", "GPU band split: full-res FP32 L/D3/M/H (Reference) vs half-res FP32 (L, D3) band split with full-res FP32 (M, H) (RealTime)", false, true },
			{ "SC-06", "Optional 16-bit normalized integer RealTime bands (RG16 UNORM, BTB/WTW [-0.25, +1.25] affine map) for sourceBits <= 12 (never FP16; >12-bit & Reference stay FP32)", false, true },
			{ "SC-07", "Optional half-res Chroma L band in RealTime (Chroma M and H remain 100% full coded resolution)", false, true },
			{ "SC-08", "Optional 50% checkerboard pixel subsampling in cell-moment stats (p2a_cell_stats / c2a_cell_stats)", false, true },
			{ "SC-09", "Optional bilinear reference warp in RealTime (vs 4x4 Catmull-Rom bicubic)", false, true },
			{ "SC-10", "Optional adaptive K budget protection in RealTime (4K60 auto K=1, Role-Adaptive K=0/1/2, Coverage-Adaptive K=2->1 with hysteresis)", false, true }
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

		// Stage A4-1 ("Climb to A4" Modular Per-Region Tailoring — all strictly DEFAULT OFF):
		// - enableRegionTailoring (td_region_tailoring): continuous C^1 Hermite smoothstep per-region band modulation
		//   (P11 focus confidence, P12 texture density, kEdgeMap edge strength, P10g grain level) + Pass 3A edge-aware blending.
		// - bokehMatchDelta (td_bokeh_match_delta): matched-focus bokeh gating tolerance on |focus_ref - focus_cur| in [0, 0.5];
		//   0.0 = OFF (baseline preservation); > 0 vetoes mismatched high-band import and allows matched-depth L-band stabilization.
		// - enableRegionDeband (td_region_deband): direct L-band region-aware temporal debanding on flat/shallow-gradient
		//   regions bounded by the quantization step, publishing continuous guidance to td.maps (kStability + kBandMask bit 3).
		bool enableRegionTailoring = false;
		double bokehMatchDelta = 0.0;
		bool enableRegionDeband = false;

		// Pass 3A — 100% Native Direct3D 11 CS 5.0 RealTime Speed & Budget-Protection Levers (all strictly DEFAULT OFF):
		//   - enableNorm16RealTimeBands (SC-06): 16-bit normalized integer (RG16) band storage for Preset::RealTime & sourceBits <= 12
		//     (incl. 12-bit tunneled DV; BTB/WTW-safe [-0.25, +1.25]*peak; >12-bit & Reference stay strictly FP32; NEVER FP16!).
		//   - enableHalfResChromaL (SC-07): half-res Chroma L band in RealTime (Chroma M & H stay 100% full coded resolution).
		//   - enablePhaseATileEarlyOut: zero-loss Phase A skips (bars outside activeRect, motion-rejected/face/zero-weight tiles,
		//     clean-peak K=0 passthrough, skip redundant 4K outClean/carryClean writes when unused).
		//   - enableBandRingCache: 3-frame decoded-frame band ring cache keyed by (srcTex, seq, w, h, activeRect, format) —
		//     splits each unique frame once instead of 3x; hard-flushed on cut/resync/size/preset change.
		//   - enableLeanChromaLumaReuse: true lean chromaReuseLumaWeights fast-path reusing Luma motionGate + cellWeights +
		//     cellDiagC directly and skipping Chroma C2A / C2EB / C2B dispatches + GPU-CPU sync stall completely.
		//   - enableCheckerboardCellStats (SC-08): 50% checkerboard pixel subsampling in p2a_cell_stats / c2a_cell_stats.
		//   - enableBilinearHardwareWarp (SC-09): bilinear warp option vs 4x4 Catmull-Rom bicubic in RealTime.
		//   - enableAuto4k60SingleRef (SC-10a): automatic K=2 -> K=1 cap when w*h >= 3840*2160 && sourceHz >= 50 Hz.
		//   - enableRoleAdaptiveK (SC-10b): K=0 clean confirmed peak, K=1 shallow dip (Reference), K=2 deep valley (NonReference).
		//   - enableCoverageAdaptiveK (SC-10c): activeTileFraction with [0.75, 0.625] hysteresis capping K=2 -> K=1 visibly.
		bool enableNorm16RealTimeBands = false;
		bool enableHalfResChromaL = false;
		bool enablePhaseATileEarlyOut = false;
		bool enableBandRingCache = false;
		bool enableLeanChromaLumaReuse = false;
		bool enableCheckerboardCellStats = false;
		bool enableBilinearHardwareWarp = false;
		bool enableAuto4k60SingleRef = false;
		bool enableRoleAdaptiveK = false;
		bool enableCoverageAdaptiveK = false;

		// Pass 3B — Forensic Blind Keyframe / Peak Detector (TD-GOP-PEAK-V2, strictly DEFAULT OFF):
		//   - enableForensicGopPeakV2: master toggle for forensic blind peak detection & Mini-GOP cadence lock.
		//   - enablePeakV2SkipGrainFreeze: Signal 1 — SKIP-block / temporal grain-freeze fraction vs P10g noise floor.
		//   - enablePeakV2AcSparsity: Signal 2 — grain-free 8x8 AC transform sparsity (pi_0, immune to HEVC/AV1 deblocking).
		//   - enablePeakV2InnovationBidir: Signal 3 — motion-compensated clean-band innovation reset + bidirectional asymmetry.
		//   - enablePeakV2CadenceLock: Signal 4 — U7 post-cut I-frame seeding + nested Mini-GOP (4/8/16) rhythm lock.
		bool enableForensicGopPeakV2 = false;
		bool enablePeakV2SkipGrainFreeze = true;
		bool enablePeakV2AcSparsity = true;
		bool enablePeakV2InnovationBidir = true;
		bool enablePeakV2CadenceLock = true;

		// Pass 3C — Encoder-DPB Keyframe Detail Bank & 3-Layer Agreement-Gated Carry (strictly DEFAULT OFF):
		//   - enableDpbDetailBankV2: upgrades anchored carry into a shot-scoped persistent Keyframe Detail Bank
		//     with zero-drift composed camera homography, progressive 1/sigma^2 grain-free Kalman accumulation,
		//     Layer 1 coarse-band (L) 2-vs-1 agreement voter, and Layer 3 Salvi 3x3 neighborhood envelope clamp.
		//     Default OFF -> legacy enableAnchoredCarry stays 100% bit-identical.
		//   - enableDpbHomographyTracking: single-resample composed H_{key->t} on Camera cells (8-32+ frames).
		//   - enableDpbKalmanAccum: progressive precision-weighted (1/sigma^2) grain-free Kalman update on static/agreed cells.
		//   - enableDpbAgreementVoter: Layer 1 coarse-band (L) 2-vs-1 agreement voter before joint covariance solve.
		//   - enableDpbNeighborhoodEnvelopeClamp: Layer 3 Salvi 3x3 min/max+variance clamp strictly inside quant-cell bound.
		//   - maxDpbBankAgeFrames: max age in unique source frames for DPB detail bank (default 32 = 4 * kContractBackRefs).
		bool enableDpbDetailBankV2 = false;
		bool enableDpbHomographyTracking = true;
		bool enableDpbKalmanAccum = true;
		bool enableDpbAgreementVoter = true;
		bool enableDpbNeighborhoodEnvelopeClamp = true;
		int maxDpbBankAgeFrames = 4 * kContractBackRefs;

		// Measured source rate in Hz (used to express any temporal horizon in seconds, never hardcoded frame counts).
		double sourceHz = kDefaultSourceHz;

		bool IsEnabled() const { return mode != Mode::Off; }
		bool ModifiesPixels() const { return mode == Mode::On; }
	};

	// Exact guard for SC-06 16-bit normalized integer RealTime bands (NEVER activates in Reference or sourceBits > 12).
	inline bool ShouldUseNorm16Bands(const Config& cfg)
	{
		return cfg.preset == Preset::RealTime &&
		       cfg.enableNorm16RealTimeBands &&
		       cfg.sourceBits > 0 &&
		       cfg.sourceBits <= kNorm16BandsMaxSourceBits;
	}

	// Deterministic stateful hysteresis controller for Coverage-Adaptive K (SC-10c: enter >= 0.75, exit <= 0.625).
	struct CoverageAdaptiveKState
	{
		bool capped = false;
		double lastActiveTileFraction = 0.0;

		void Reset()
		{
			capped = false;
			lastActiveTileFraction = 0.0;
		}

		bool Update(double activeTileFraction)
		{
			const double frac = (std::isfinite(activeTileFraction) && activeTileFraction >= 0.0)
				? (activeTileFraction > 1.0 ? 1.0 : activeTileFraction)
				: 0.0;
			lastActiveTileFraction = frac;
			if (!capped && frac >= kCoverageAdaptiveEnterFraction)
				capped = true;
			else if (capped && frac <= kCoverageAdaptiveExitFraction)
				capped = false;
			return capped;
		}
	};

	// Unified Pass 3A effective-K cap decision shared 1:1 between CPU (TdV2ReferenceEngine) and GPU (TdV2GpuEngine).
	// frameRoleInt matches QualityPattern::FrameRole (0=Unknown, 1=Peak, 2=Reference, 3=NonReference) without adding a header include.
	struct AdaptiveKDecision
	{
		int maxExternalRefs = 3; // 0 = clean-peak K=0 passthrough, 1 = single-ref K=1 cap, 2+ = unrestricted
		bool auto4k60Active = false;
		bool roleAdaptiveReduced = false;
		bool coverageAdaptiveCapped = false;
		double activeTileFraction = 1.0;
	};

	inline AdaptiveKDecision ResolveAdaptiveKCap(
		const Config& cfg,
		int w,
		int h,
		int frameRoleInt,
		bool isConfirmedPeak,
		double activeTileFraction,
		CoverageAdaptiveKState& coverageState)
	{
		AdaptiveKDecision d;
		d.activeTileFraction = (std::isfinite(activeTileFraction) && activeTileFraction >= 0.0)
			? (activeTileFraction > 1.0 ? 1.0 : activeTileFraction)
			: 0.0;

		// Only RealTime (or explicit opt-in toggles) activate Pass 3A adaptive-K caps; default all-OFF returns maxExternalRefs = 3.
		if (cfg.enableRoleAdaptiveK)
		{
			// FrameRole::Peak == 1 (or explicit isConfirmedPeak) -> K=0 clean peak passthrough;
			// FrameRole::Reference == 2 -> K=1 shallow dip; FrameRole::NonReference == 3 / Unknown -> K=2 deep valley.
			if (isConfirmedPeak || frameRoleInt == 1)
			{
				d.maxExternalRefs = 0;
				d.roleAdaptiveReduced = true;
				return d;
			}
			if (frameRoleInt == 2)
			{
				if (d.maxExternalRefs > 1)
					d.maxExternalRefs = 1;
				d.roleAdaptiveReduced = true;
			}
		}

		if (cfg.enableAuto4k60SingleRef &&
		    static_cast<int64_t>(w) * static_cast<int64_t>(h) >= static_cast<int64_t>(kAuto4k60MinPixels) &&
		    std::isfinite(cfg.sourceHz) && cfg.sourceHz >= kAuto4k60MinHz)
		{
			if (d.maxExternalRefs > 1)
				d.maxExternalRefs = 1;
			d.auto4k60Active = true;
		}

		if (cfg.enableCoverageAdaptiveK)
		{
			if (coverageState.Update(d.activeTileFraction))
			{
				if (d.maxExternalRefs > 1)
					d.maxExternalRefs = 1;
				d.coverageAdaptiveCapped = true;
			}
		}
		else
		{
			coverageState.lastActiveTileFraction = d.activeTileFraction;
		}

		return d;
	}

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

	// Active picture rectangle [x0, x1) x [y0, y1). Bar pixels outside are bit-exact copied and never crossed by warps.
	struct ActiveRect
	{
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

		static ActiveRect Full(int w, int h) { return { 0, 0, w, h }; }

		ActiveRect ClampedTo(int w, int h) const
		{
			ActiveRect r;
			r.x0 = (std::max)(0, (std::min)(w, x0));
			r.y0 = (std::max)(0, (std::min)(h, y0));
			r.x1 = (std::max)(r.x0, (std::min)(w, x1));
			r.y1 = (std::max)(r.y0, (std::min)(h, y1));
			return r;
		}

		int Width() const { return x1 - x0; }
		int Height() const { return y1 - y0; }
		bool Contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
		bool Empty() const { return x1 <= x0 || y1 <= y0; }
		bool IsValid(int w, int h) const { return x0 >= 0 && y0 >= 0 && x1 > x0 && y1 > y0 && x1 <= w && y1 <= h; }
	};

	// Original pathway law: forces TD strictly Off so Original == Bill bit-exact.
	inline void ApplyOriginalPathwayOverride(Config& cfg)
	{
		cfg.mode = Mode::Off;
	}
}
