# Temporal Detail v2 — Approved Specification Addendum (`TD-U0`..`TD-U5`, Phase 6, Owner Decisions `D1`–`D7`)

- **Date:** 2026-11-25
- **Branch:** `gemini/td` (base `02c42348c`, W1 ancestor `bfd055d1f`)
- **Authoritative sources:**
  - `C:\dev\gemini-handoff\queue\06-OPTIONAL-only-if-W23-stalls\2026-11-25_request-TD-v2-upgrade-BUILD.READY.md` (Owner Decisions `D1`–`D7`)
  - `C:\dev\gemini-handoff\designs\2026-11-25_td-v2-upgrade-plan-FABLE52-draft.md` (Upgrade Plan `U0`..`U7`, Phase 6)
  - `C:\dev\gemini-handoff\reviews\2026-11-25_td-v2-FABLE52-audit.md` (Audit Findings `B-1..2`, `H-1..6`, `M-1..9`, `L-1..5`)
  - Upstream specs in `D:\codex\vp-handover\`: `TD-INDEX.md`, `99-td-design-plan.md`, `refs\111-td-blend-math.md`, `refs\99c-td-method-search.md`, `principles\temporal-detail.md`

---

## 1. Scope & Modular Toggle Principle (Owner Executive Order 2026-11-25)

Every upgrade in `TD-U1..TD-U5` and Phase 6 is modularly toggleable in `TdV2::Config` (`TdV2Config.h`) and exposed in the VP-V Settings UI using proper select controls (`K::Toggle`, fixing `L-5`), so real-clip diagnostic/ablation (`TD-U0`) and RTX 3050 Ti timing (`TD-U6`) can isolate every mechanism without code edits:

- **Main VP-V page (`VP-V > Repair > Main > Temporal detail`):** `temporal_detail` (`off` default | `shadow` | `on`), `td_limit_kappa` (`1.0` default, range `1.0..3.0`), `td_mode` (`cell` default | `strict`), `td_overlay` (`off` default | `weights` | `maps` | `focus` | `motion`).
- **Advanced / Developer card (`VP-V > Repair > Advanced > Temporal detail: tuning`, locked by `temporal_detail`):** `td_solver`, `td_quality_signal`, `td_stats_pooling`, `td_anchored_carry`, `td_smooth_warp`, `td_hard_fb_gate`, `td_chroma_weights`, `td_smooth_grain_guard`, `td_subpixel_debias`, `td_edge_upsample`, `td_neighborhood_clamp`, `td_region_tailoring`, `td_bokeh_match_delta`, `td_region_deband`.
- **Original pathway & off-state invariance:** `temporal_detail = off` and the `Original` pathway keep all TD v2 modules completely inert (`TdProducts::kEngineRequestedToday = false`, library-ready until Wave 4–6 integration).

---

## 2. Housekeeping (`TD-U0-HOUSEKEEPING`, `M-5`)

- Non-TD Q9a/Q9b near-black sigma-delta dither / `DitherNeedSeam` / `CoreSegments.inl` changes were cleanly split onto local branch `gemini/td-dither-split` from W1 base `bfd055d1f` and reverted from `gemini/td`.
- `reviews/2026-11-23_td-v2-summary.md` explicitly labels TD v2 as **library, unproven** until real-clip diagnostic (`TD-U0`) and RTX 3050 Ti timing (`TD-U6`) pass.

---

## 3. Exact Constrained QP Blend Solver (`TD-U2`, `H-2`, `refs/111-td-blend-math.md`)

- Replaces the legacy unconstrained inverse + clip-to-zero + rescale solver with an exact active-set constrained QP solver for:
  $$\min_{w \in \mathbb{R}^K} w^\top C w - 2 w^\top c \quad \text{s.t.} \quad w_k \ge 0,\ \sum_{k=1}^K w_k \le 1$$
- For $K \le 4$, enumerates all $2^K - 1 \le 15$ non-empty active subsets (plus the $w = 0$ candidate and the exact KKT simplex face $\sum_{k \in S} w_k = 1$ via Lagrange multiplier $\lambda = (\mathbf{1}^\top C_S^{-1} c_S - 1) / (\mathbf{1}^\top C_S^{-1} \mathbf{1})$ whenever an interior solution exceeds unit sum), using the diagonal regularizer $0.01 \cdot \text{mean}(\text{diag}(C))$.
- Implemented bit-consistently across CPU (`TdV2BlendMath.h`, `TdV2ReferenceEngine.h`, `TdV2Chroma.h`) and GLSL Pass 2B / Pass C2B (`TdV2Shaders.h`, `TdV2Gpu.h`, `TdV2ChromaGpu.h`).
- Selectable via `Config::solverMode` (`SolverMode::ActiveSetExact` default vs `SolverMode::LegacyClipRescale`, UI key `td_solver`).

---

## 4. Dual Picture-Only Quality & Own-Error Estimators (`TD-U1`, `B-2`, Owner `D1`)

To support HEVC/AV1 and rescaled streams where fixed 8-pixel block edges are absent or unaligned, two grid-free picture-only estimators are provided alongside the legacy optional AVC/MPEG-2 cue (`Config::qualitySignalMode`, UI key `td_quality_signal`):

1. **`QualitySignalMode::AlignedSharpnessA2e` (default):**
   - Measures motion-aligned relative sharpness $r_t = \text{median}_{\text{trusted}} \ln(E_t / E'_{t-1})$ on grain-free mid+fine bands, debiased for sub-pixel bicubic warp attenuation (`FineBandBicubicWarpVarianceGain`), abstaining focus-flagged cells and preserving exact-copy/cadence handling.
   - Integrates running sum $S_t$ with rolling window median removed to classify `Peak` / `Valley` / relative rank.
   - Computes empirical own-error prior `A2e` per band (`L, M, H`) and activity class (`flat` vs `textured`) from the robust median of grain-corrected instrumental-variable cross-moments $c_{\text{IV}} = \mathbb{E}[d_p d_n]$, capped by a **time-domain** (seconds, never hardcoded frame count) rolling median of recent same-role frames and frozen on cut/fade/resync flags.
2. **`QualitySignalMode::NonBlockPictureStep` (Owner `D1(b)`):**
   - Grid-free coding-step estimator (`TdV2NonBlockStepEstimator`) derived from quantization physics (flat-region quantization comb step + fine-band dead-zone drop) with zero reliance on `x % 8 == 0` block coordinates and zero arbitrary clip-fitted constants.
3. **`QualitySignalMode::LegacyBlockEdge8px` (optional ablation/bonus cue):**
   - Retains the legacy 8-pixel block-edge cue for explicit head-to-head ablation on AVC/MPEG-2 sources.

Both estimators are wired identically on CPU (`TdV2FrameRole.h`, `TdV2ReferenceEngine.h`) and GPU (`ComputeQualityEvidenceGpu` in `TdV2Gpu.h` / `TdV2Shaders.h`).

---

## 5. Honest Adjustable Output Limit (`TD-U3`, `H-1`, `M-1`, `M-6`, Owner `D3`, `L-5`)

- **Default strict statistical cap (`kappa = 1.0`, adjustable `[1.0, 3.0]`):**
  - On transform-unknown streams (HEVC, AV1, rescaled), applies **both**:
    - Per-band bound: $|\Delta_j(p)| \le \kappa \sqrt{U_j(\text{block})}$ using exact `BandFromBase` transfer gains $\|H_b\|_2^2$ and shrunk per-block own error $U_j$ (removing the filter-peak `1.0` full-variance amplification).
    - **Joint cross-band bound** (`ApplyJointCrossBandBound`): $\left|\sum_j \Delta_j(p)\right| \le \kappa \sqrt{\sum_j U_j(\text{block})}$, scaling all bands uniformly when the combined correction exceeds the joint bound, across both CPU and GLSL Pass 3.
  - Default `Config::pixelBoundKappa = 1.0`, adjustable via Main UI key `td_limit_kappa` (`1.0 .. 3.0`).
- **Honest status & no fake encoder prediction:**
  - Removed the warped-peak `refs[0].clean` substitute for missing `codecModelPb` on CPU and GPU; when real encoder prediction is absent, the engine visibly enforces the statistical pixel-domain bound.
  - Ctrl+I status formatter (`TdV2Settings.h`) explicitly reports `"limit: statistical (transform unknown)"` vs `"limit: cell (AVC 8x8)"`, active `kappa`, and shows `cell`/`strict` as `"inactive: transform unknown"` whenever transform geometry is unknown (`M-1`).
- **Perceptual & AQ tightening (`M-6`):**
  - PQ perceptual step weight is capped at `kPqStepWeightMax = 1.0` (may only tighten, never widen beyond `1.0`).
  - AQ `dQP` step widening is gated OFF by default (`enableAqStepScaling = false`) until a live bitstream AQ producer exists.
  - FocusGuard 3-sigma threshold is decoupled from `pixelBoundKappa`.

---

## 6. Steadier Statistics & Flicker Controls (`TD-U4`, `H-3`, Owner `D4`)

- **Band-dependent spatial pooling** (`StatsPoolingMode::BandShrink` default, UI key `td_stats_pooling`):
  - Pools cross-moments over `5x5` cells on Low (`L`), `3x3` cells on Mid (`M`), and `3x3` (Reference) or `1x1` (Real-Time) on High (`H`), restricting `SC-01` (`1x1`) to `H` only.
- **Empirical-Bayes shrinkage (`TdV2BlendMath.h`, GLSL Pass 2-EB `kPass2EbPriorReduce`):**
  - Subtracts grain variance **before** shrinkage; keeps exact-copy frames at zero; shrinks cell-level $c_{\text{IV}}$ toward the band/activity prior using derived sampling variance (`EffectiveSamplesForBand`, `SamplingVarianceOfCrossMoment`, `EmpiricalBayesShrinkC`), clamped to $[0, \min(C_{pp}, C_{nn})]$.
  - GPU reduces the compact `3x2 RGBA32F` `ebPriorTex_` prior texture deterministically before Pass 2B.
- **Optional 2-frame same-role statistics-only pooling (OFF by default, Owner `D4`):**
  - `enableTwoFrameSameRoleStatsPool = false` (`td_stats_pooling = two_frame_same_role`). Averages **only** second-order statistics ($C, c$) across the previous confirmed same-role frame—never pixels or weights—and resets immediately on cut, fade, or resolution change.

---

## 7. Anchored Carry, Real `kDrift`, Phase 6 Switches & GPU Late Carry (`TD-U5`, `H-4..6`, `M-2..9`, `L-1..4`, Owner `D2`, `D5`)

- **Carry-forward OFF by default (`D2`):**
  - Both `enableCarryForward` and `enableAnchoredCarry` default to `false` (`td_anchored_carry = off`).
- **Single-resample anchored carry (`H-4`, `H-5`):**
  - When enabled, anchors strictly to the last confirmed peak (`maxAnchorAgeFrames = 8`, `PrevPeak2` role for `A1`), warping once from the anchor rather than chaining resamples.
  - Per-cell held-out structure check (mid-band NCC invariant to fine detail loss + coarse-band mean difference within the U3 limit): failing cells receive weight `0` and are flagged for re-seed.
- **Real `kDrift` measurement (`M-2`, `UPG-BLKR-3`, `UPG-HIGH-2`):**
  - Measured at confirmed peaks via direct vs composed flow (or seeded $\pm 2\text{ px}$ sub-pixel bilinear block refinement tagged visibly as `"drift: reduced"` / `StandInKind::LongPairDrift`), published in `td.maps kDrift` on BOTH CPU and GPU (`kPassAnchorDriftAudit`), barring cells with `kDrift > kAnchorDriftBarPx` (`0.25 px`, derived from P3r's quarter-pel floor) for the next span.
- **Phase 6 modular switches (CPU & GLSL, luma & chroma):**
  - `enableSmoothWarpVectors = true` (`6.1` / `L-1`, `td_smooth_warp`): bilinear vector interpolation between 16x16 cell centres (requiring matching `RegionMotionClass` across all 4 corners).
  - `enableHardFbOcclusionGate = true` (`6.2` / `M-3`, `td_hard_fb_gate`): hard-zeros reference weights when forward/backward disagreement exceeds `kHardFbOcclusionPx = 1.0 px` on both luma and chroma CPU/GPU.
  - `chromaReuseLumaWeights = false` (`6.6`, `td_chroma_weights`): independent chroma weights by default, or reuse luma cell weights mapped strictly by matching `RefRole` (disabled on Branch A `Unidentified`, preserving Layer-4 `cellRetainedGrain`, zero-clearing GPU diagnostics when C2A/C2B are skipped, and gating Pass C3 on `motion_gate` rather than `ownW <= 0`).
  - `enableSmoothLayer4Guard = true` (`L-3`, `td_smooth_grain_guard`): Pass 4B / Pass C4B bilinear grain add-back using dead `sc.warpedVal` / `sc.warpedUv` to prevent SRV/UAV aliasing with zero extra VRAM.
  - `enableGpuCounterSummary = true` (`M-9`): compact 2-texel (32-byte) `counterSummaryTex_` live summary readback in zero-copy GPU mode.
- **Shared core & time-domain hygiene (`M-7`, `L-2`):**
  - `TdV2Chroma.h` directly includes `<VP-V/core/ChromaSiting.h>` and delegates `SitingSpec::Resampled` to `ChromaSiting::ShiftAfterResample`.
  - `kAnchorCanvasTauSec = 1.0 / 3.0` (source seconds, rate-independent).
- **GPU late-carry controller (`D5`, `TdV2GpuLateCarry.h`):**
  - GPU-side `TdV2GpuLateCarryController` on `pl_tex` (warp + linear decay `2/3 -> 1/3 -> 0` using un-decayed `next_delta` + `Overlay`/`DynamicTexture` zeroing + opposite-sign/hull/Huber/U3-kappa guards + explicit stale-sequence late-arrival guard `OnLateResultArrived` + destructor `pl_tex_destroy` cleanup + `MakeVramUser` persistent VRAM budgeting).

---

## 8. Chroma Reduction Fidelity & Cross-Model Audit Closures (`CHR-HIGH-1..4`, `UPG-BLKR-1..4`, `UPG-HIGH-1..6`)

- **Delivered-to-coded chroma reduction (`CHR-HIGH-4`):**
  - `TdV2Chroma.h` / `TdV2ChromaGpu.h` reduce delivered 4:4:4 / RGB chroma to the identified coded subsampling grid using sited 4-tap Catmull-Rom resampling (`SampleSitedActive` / `kPassChromaReduce`). Catmull-Rom is a fast, local interpolating point-sampler rather than the exact linear-algebraic left-inverse $D(U(x)) = x$ of the upstream chroma upsampler $U$; when the delivered grid already matches the coded YUV grid (native 4:2:0 / 4:2:2 / 4:4:4), reduction is an exact bit-identical identity with zero resampling passes.
  - Anchor canvas referencing (`ChromaProcessInput::enableAnchorCanvasRef`) defaults to `false`, is strictly gated on `cfg_.enableAnchoredCarry && in.enableAnchorCanvasRef` (`D2`), and runs a per-cell held-out mid-band NCC + coarse-band mean-difference check before accepting anchor canvas cells.
  - Carried-CfL OLS ridge uses derived chroma noise variance `max(a2Stats.totalVar, lsbQuantVar)` rather than a hardcoded `1/12` floor.
- **Chroma `A2e` / EB / `U3` & Bokeh / Focus Parity (`CHR-HIGH-1/2`, `CHR-MEDIUM-1..3`, `CHR-LOW-1/3`):**
  - Both CPU `TdV2Chroma.h` and GPU `TdV2ChromaGpu.h` run band-dependent pooling (`PoolRadiusForBand`), coded-chroma `EffectiveSamplesForBand`, `EmpiricalBayesShrinkC`, `A2ePriorHistory` (holding rolling median on one-sided `K = 1` frames and opening the multi-ref `A2e + A3` upper envelope to `U_j + 3*tau` while keeping `kappa * sqrt(U_j)` output bounds honest), `hPoolR` own-grain pooling, standalone chroma rack-focus shift fallback when `explicitFocusChangeCells` is absent, flat-cell unsplit grain protection when `retainedGrainVar == 0`, and bokeh-boundary straddle protection (CPU `codedDeltaLow` + GPU `td_sample_coded_delta_sited` defocused-tap masking) so sharp `M`/`H` coded deltas never bleed 1–2 px into bokeh.
- **Luma & GPU Audit Closures (`UPG-BLKR-1/3/4`, `UPG-HIGH-1..6`, `UPG-MED-1..3`, `UPG-LOW-1`):**
  - Fixed `QualityPattern::FrameRole` mapping (`Peak=0`, `Reference=1`, `NonReference=2`, skipping `Unknown`/`ExactCopy`) and gated 2-frame same-role stats pooling on matching `RefRole`, age `<= 1 s`, and `|v| < 0.5 * cellPx`.
  - Split oversized raw GLSL literals (`kPass2SolverHeader`, `kPass2MergeAndSolve`, `kPassChromaEbPriorReduce`, `kPassChromaMergeSolve`) so every raw GLSL literal across `v2/*.h` stays `<= 12,090 B` CRLF (well below MSVC's `16,380 B` `C2026` limit).
  - Replaced the bogus `E_H / E_M == 1.345` `dzStep` and 4-px stride in `TdV2NonBlockStepEstimator` / `kPassQualityEvidenceReduce` with a per-row/per-column monotonic 3-px-plateau quantization staircase check excluding `Overlay` cells and aggregating by median.
  - Added GPU `kPassAnchorHeldOutCheck` + `kPassAnchorDriftAudit` writing real `kDrift` into `td.maps slice2.z`, wired `ebPriorTex_` into GPU `kPass2MergeAndSolve` (always zero-initialized when EB is off), and prevented `anchorAgeFrames_ <= 1` self-instrumentation vs `prevPeak`.

---

## 9. Stage A4-1 — Modular Per-Region Tailoring ("Climb to A4", All Default OFF)

All three Stage A4-1 controls are strictly modular (`Config` defaults `false` / `0.0`), exposed as proper select (`K::Toggle`) controls on `VP-V > Repair > Advanced > Temporal detail: tuning` (locked by `temporal_detail`), preserve bit-identical default/Original behavior, and achieve `<= 1e-3` CPU (`TdV2ReferenceEngine.h`) vs GPU (`TdV2Shaders.h` + `TdV2Gpu.h`) parity within D3D11 CS 5.0 limits (`<= 16` SRVs, `< 32 KB` LDS, strictly FP32):

1. **Continuous Per-Region Smoothstep Management & Pass 3A Edge-Aware Blending (`td_region_tailoring = off | on`, default `off`):**
   - Evaluates continuous `[0, 1]` per-cell region descriptors (`EvaluateCellRegionDescriptors`) using canonical $C^1$ cubic Hermite smoothsteps (`SmoothstepC1`) derived strictly from `BandFromBase` transfer gains and quantization physics (`+/- 3 dB` SNR brackets `[0.5x, 2x]` and `1..3 sigma` noise-floor ramps; Rule 1):
     - `P11` focus confidence (`focusConf`), `P12` texture density (`textureDensity`), `kEdgeMap` edge strength (`edgeStrength`), and `P10g` grain level (`grainLevel`), accepting optional shared upstream maps or deriving them internally with visible `StandInKind::RegionMapsDerived` notice.
   - Replaces hard binary flat/textured activity switching with continuous `P12` blending of `ebPriorU`/`ebTauSq`, and modulates `M`/`H` band weights continuously (`regionBandGain = {1, focusConf, focusConf * (1 - grainLevel * (1 - textureDensity))}`).
   - **Pass 3A Edge-Aware Blending (`SpatialEdgeAwareBilinearWeight` / `kPass3BlendAndBound`):** Stops cross-edge temporal weight bleed across neighboring 2×2 cells using BOTH clean-luma range affinity and the $C^1$ edge barrier `(1 - pxEdge) * (1 - SmoothstepC1(1, 3, |m_q - m_own| / sigPair))`, keeping smooth interiors seamless with zero cell-grid blocking and sharp silhouettes halo-free.
2. **Matched-Focus Bokeh Tailoring (`td_bokeh_match_delta = off | 0.125 | 0.25 | 0.375`, default `off`):**
   - Tolerance in `[0, 1]` focus space (elementary dyadic fractions `1/8, 1/4, 3/8`, bounded `<= 1/2`) on per-reference focus delta `|focus_ref - focus_cur|`.
   - On rack-focus mismatch (`|focus_ref - focus_cur| >= td_bokeh_match_delta`): vetoes cross-frame import (`matchGate = 1 - SmoothstepC1(0.5*delta, delta, focusDelta) == 0`) BEFORE QP solve so sharp edges are never imported into defocused bokeh or vice versa.
   - On matched out-of-focus depth (`|focus_ref - focus_cur| < td_bokeh_match_delta`): allows clean `L`-band stabilization while keeping `M = H = 0` so bokeh is never smeared or falsely sharpened.
3. **Direct `L`-Band Region-Aware Debanding (`td_region_deband = off | on`, default `off`) -> `td.maps`:**
   - Identifies flat/shallow-gradient banded regions via `(1 - edgeStrength) * (1 - textureDensity) * (1 - grainLevel) * (1 - SmoothstepC1(quantStep, 3 * quantStep, cellStdCleanY))`.
   - Applies step-consistent MMSE `L`-band temporal averaging (`agree_k / (1 + sum agree)` where `rmsDiff <= quantStep`) bounded strictly by the quantization half-step `debandApplied * 0.5 * quantStep` (respecting Layer-2/Layer-4 grain protection and `fadeKeep`).
   - Publishes continuous `[0, 1]` deband coordination guidance into U19 `td.maps` (`kStability = max(baseStability, debandApplied)` read directly by `TdMapsView::Reader::Deband`, plus packed reason bit `kTdMapsBandMaskRegionDebandBit = 8` in `kBandMask`, readable via `ReadRegionDebandGuidanceFromMaps`) without altering `TdMapsView::kUnitVersion` (`1`) or field count (`12`).
