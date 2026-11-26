<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Small Fast Objects & Trajectory-Type Study (`fast_objects/`)

Unit: `motion/estimation` (sub-folder `fast_objects/`, submodule of slot `motion.estimation`, experimental side branch `gemini/exp-fast-objects`).

Physics-derived, zero-neural-network detector, multi-hypothesis trajectory classifier, and masked sub-pixel refiner for small fast moving objects (sports balls, pucks) over panning/zooming cameras. Built on shared VP-V motion products (`P1` half-luma, `P4` source stats/fade, `P7`/`P9` camera homography, `P8` labels + `CE-WATER`, `P10g` grain variance, `kEdgeMap`, `MotionAhead` look-ahead window up to `VpSkeleton::kAheadCeiling = 6`).

## Contract & Invariants

- **Default OFF & `Original == Bill` invariance:** `FastObjects::Config::enabled` defaults strictly to `false`. When `enabled == false` or `originalPathway == true`, `FastObjectTracker` returns immediately with `0` work, `0` VRAM, `SchedulerMode::Off`, and explicit `AbstainReason::Disabled` / `AbstainReason::OriginalPathway`.
- **Zero silent fallbacks (Rule 4):** Every abstention, downgrade, overflow, causal fallback, coasting state, crossing ambiguity, hotspot suppression, and centroid-vector fallback is surfaced through explicit bitmasks, diagnostic counters, and `FastObjectTrackSet::FormatStatus()`.
- **Resource budget target (4K60 add-on):** `~0.03-0.06 ms` idle (`Dormant` scheduler after `N_idle = 6` quiet frames), `~0.20-0.35 ms` active (`ActiveT1`), `< 0.05 ms` CPU tracking, zero steady-state heap allocations (caller-owned workspaces & fixed-capacity track arrays), clean budget shedding (`ActiveT2 -> ActiveT1 -> Dormant -> ShedBudget`) via `SetBudgetTier()` before any core video processing stage.
- **Coordinate conventions:**
  - `CameraModel::Params` homographies operate in picture-centred source px (`CameraMotion.h`).
  - `BlobCandidate::cx/cy`, `FastObjectTrack::cx/cy`, `maskBox`, `Capsule`, and `FastObjectOverlay` use top-left active-frame source px (`[0, srcW) x [0, srcH)`).
  - `FastObjectTrack::vx/vy` is the exact `t -> t+1` pair displacement in source px; `velXPerSec/velYPerSec` and `ax/ay` are in physical units (`source px/s` and `source px/s^2`) using measured inter-frame `PairTiming::Seconds`.

## Derived Physics & Statistical Constants (Rule 1)

| Constant | Value / Formula | Physical / Statistical Derivation |
|---|---|---|
| `kSeedSigma` | `4.0` | Standard `4 sigma` Bonferroni bound for `~10^6` half-res pixels; bivariate normal (`rho = 1/2`) same-sign tail `P_FA = 1.32e-5`. |
| `kGrowSigma` | `3.0` | VP-V universal `3 sigma` contract (`CameraModel::Config::absThresholdSigmas = 3.0`), `P_FA(3.0; rho=0.5) = 4.61e-4`. |
| `kQuantizationSigmaScale` | `1 / sqrt(12)` | Uniform quantization variance law `sigma_q = delta_code / sqrt(12)`. |
| `kMadToSigma` | `1.4826022` | Normal distribution MAD scale `1 / Phi^-1(0.75)` (matches `RobustStats::kMadToSigma`). |
| `Chi2Dof3ThresholdForSigma(k)` | Exact Newton solve of `S_3(x) = erfc(k / sqrt(2))` | Exact 3-DOF `chi^2(3)` threshold for `z_Y^2 + z_Cb^2 + z_Cr^2` matching the 1-DOF `k sigma` false-alarm tail. |
| `kCellSizeHalfPx` | `4` | `4x4` half-luma tile = `8x8` source px, aligned with `P2` NVOF and `FieldRefine` block grid. |
| `kStreakPixelSigma` | `kSeedSigma / kCellSizeHalfPx = 1.0` | Exact uniform per-pixel floor of a streak triggering the `4x4` Neyman-Pearson matched filter `z_cell >= kSeedSigma`. |
| `kMaxActiveCells` | `256` | Bounded `(256 + 2) x 16 B = 4,128 B` GPU->CPU staging cap (header + active cells + MAD noise slot); overflow triggers explicit `AbstainReason::ClutterOverflow`. |
| `kMinBlobAreaHalfPx` | `4` | `2x2` Nyquist sampling minimum at half resolution. |
| `kMaxMinorAxisFrac` | `0.03` | `3%` of active picture height (32 px @ 1080p, 64 px @ 2160p); applied to deconvolved minor axis `2 * r_est` so long motion streaks are never rejected. |
| `kMaxSpeedWidthFracPerSec` | `2.0` | `2.0 * W_pic px/s` (covers a 160 mph serve crossing a broadcast court in 0.5 s). |
| `r_est, L_est` | `2 * sqrt(lambda_2)`, `sqrt(12 * max(0, lambda_1 - lambda_2))` | Exact inversion of uniform disc (`r^2/4`) convolved with 1D boxcar motion-blur PSF (`L^2/12`), with Sheppard's `s^2/12` pixel-cell variance correction. |
| `kElongationRatioMin` | `sqrt(4 / 3)` | Exact threshold where `L_est > r_est` (`lambda_1 / lambda_2 > 4/3`). |
| `kChi2Dof2_99` | `-2 * ln(0.01) = 9.21034` | Exact 99% `chi^2(2)` kinematic innovation gate. |
| `Chi2Critical99(dof)` | Exact regularized upper gamma inverse `Q(dof/2, x/2) = 0.01` | Exact 99% goodness-of-fit threshold for BIC models (`2N - d` DOF). |
| `kManeuverAccelHeightFracPerSec2` | `1.0` | `1.0 * H_pic px/s^2` broadcast-court gravity acceleration scale for Taylor remainder process noise `0.5 * sigma_a * dt^2`. |
| `kDormantIdleFrames` | `6` | Equals `VpSkeleton::kAheadCeiling` (one full quiet ahead window before dropping to `Dormant`). |
| `kHotspotDecaySec` | `2.0` | Exponential decay time constant for the `64x36` clutter-hotspot auto-mask (cleared immediately on cuts/resyncs). |
| `kCoastMaxSec` | `0.200` | Maximum occlusion coasting duration (200 ms) with zero emitted vectors (`ValidityState::Coasting`). |
| `kDefaultMinConfidence` | `0.25` | Elementary ratio `1/4` (above `g_NIS(chi^2(2, 0.99)) = 0.165` so near-gate tracks become `Uncertain`, while valid centroid-fallback tiny balls remain `Valid`). |

## Modules

- `FastObjectTypes.h`: Pure C++14/17 PODs, enums, derived constants, exact `chi^2` helpers, compact 16x16 bitmask helper, and status formatter.
- `FastObjectResidualRef.h/.cpp`: Stage A CPU reference (camera-compensated 3-frame same-polarity symmetric residual, fade normalization, local noise/gradient floor with MAD fallback, optional half-res `Cb/Cr` 3-DOF `chi^2` residual, `4x4` matched-filter reduction, hysteresis connected components, moment deconvolution, P4/P8/CE-WATER guards).
- `FastObjectTrajectory.h/.cpp`: Stage B multi-hypothesis tracker (camera-stabilized window mapping with Cramér-Rao covariance propagation, exact Hungarian GNN association, BIC 4-model classifier `LinearGlide`/`Ballistic`/`Impulse`/`Clutter`, two-leg impulse vertex stitching, 2.5D perspective depth check, motion-blur tangent alignment & shutter estimator, `64x36` clutter-hotspot auto-mask, lightweight session appearance prior).
- `FastObjectRefineRef.h/.cpp`: Stage B.5 ROI-only coarse-to-fine masked ZNCC refinement (quarter -> half +/-2 -> full +/-2 -> parabolic sub-pel fit, background subtraction for disjoint movers, 1D differential motion-blur PSF synthesis, transparent `kCentroidVector` fallback).
- `FastObjectOverlay.h`: Pure header-only consumer accessors (`SampleObjectVector`, `SampleObjectWarp`, `IsProtectedForTemporalDetail`, `BuildTinyMoverRois`, `MicroRoiDeblurHint`).
- `FastObjectTracker.h/.cpp`: Stateful top-level orchestrator with Dormant/Active scheduler, `OverloadArbiter` budget shedding hook, cut/flash/resync reset, and `enabled`/`originalPathway` fast-off gates.
- `FastObjectGpu.h/.cpp`: Native Direct3D 11 (`cs_5_0`) Stage A compute mirror (3-frame same-polarity residual + optional `Cb/Cr` 3-DOF `chi^2` + `4x4` Neyman-Pearson matched-filter reduction + `InterlockedAdd` bounded `<= 256` active-cell compaction + 3-slot async staging readback ring + sparse GPU-cell blob extraction, `0` VRAM when OFF).
