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
- **Advanced / Developer card (`VP-V > Repair > Advanced > Temporal detail: tuning`, locked by `temporal_detail`):** `td_solver`, `td_quality_signal`, `td_stats_pooling`, `td_anchored_carry`, `td_smooth_warp`, `td_hard_fb_gate`, `td_chroma_weights`, `td_smooth_grain_guard`, `td_subpixel_debias`, `td_edge_upsample`, `td_neighborhood_clamp`.
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
- **Real `kDrift` measurement (`M-2`):**
  - Measured at confirmed peaks via direct vs composed flow (or seeded $\pm 2\text{ px}$ local block refinement tagged visibly as `"drift: reduced"` / `StandInKind::LongPairDrift`), published in `td.maps kDrift`, barring cells with `kDrift > 1.0 px` for the next span.
- **Phase 6 modular switches (CPU & GLSL):**
  - `enableSmoothWarpVectors = true` (`6.1` / `L-1`, `td_smooth_warp`): bilinear vector interpolation between 16x16 cell centres.
  - `enableHardFbOcclusionGate = true` (`6.2` / `M-3`, `td_hard_fb_gate`): hard-zeros reference weights when forward/backward disagreement exceeds `1.0 px`.
  - `chromaReuseLumaWeights = false` (`6.6`, `td_chroma_weights`): independent chroma weights by default, or reuse luma cell weights (skipping chroma Pass C2A/C2B).
  - `enableSmoothLayer4Guard = true` (`L-3`, `td_smooth_grain_guard`): Pass 4B bilinear grain add-back using dead `sc.warpedVal` to prevent SRV/UAV aliasing with zero extra VRAM.
  - `enableGpuCounterSummary = true` (`M-9`): compact 2-texel (32-byte) `counterSummaryTex_` live summary readback in zero-copy GPU mode.
- **Shared core & time-domain hygiene (`M-7`, `L-2`):**
  - `TdV2Chroma.h` directly includes `<VP-V/core/ChromaSiting.h>` and delegates `SitingSpec::Resampled` to `ChromaSiting::ShiftAfterResample`.
  - `kAnchorCanvasTauSec = 1.0 / 3.0` (source seconds, rate-independent).
- **GPU late-carry controller (`D5`, `TdV2GpuLateCarry.h`):**
  - GPU-side `TdV2GpuLateCarryController` on `pl_tex` (warp + linear decay `2/3 -> 1/3 -> 0` + U3 consistency clamp + explicit stale-sequence late-arrival guard `OnLateResultArrived` + visible `FallbackCatalog`/`StandInRegistry` counters).
