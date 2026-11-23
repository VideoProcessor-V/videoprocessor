# Temporal Detail v2 (`TD-S0..TD-S7`) — Implementation & Integration Summary

**Date:** 2026-11-23  
**Branch:** `gemini/td` (branched from W1 tip `bfd055d1f`; **OFF by default**, `Original == Bill` bit-exact)

---

## Plain-English Overview: What Temporal Detail v2 Does Visually & Functionally

- **What viewers see:** Compressed HDMI streams (Apple TV+, Netflix, cable, YouTube) give keyframes (I/P) far more detail than the B-frames between them, producing banded skies and dark walls, detail that softens and pulses between keyframes, and blocky colored edges. Temporal Detail v2 blends the **grain-free picture** of each frame with its motion-aligned past and future sharper peak frames (plus recurrent carry-forward), keeping every pixel adjustment inside what the received frame could have been before codec rounding, and returning the frame's **own untouched film grain** bit-for-bit.
- **Safety and cinema fidelity:**
  - **Camera pans vs. fast objects:** Camera pans stay blended cleanly, while fast-moving objects relative to the camera fade smoothly to zero with 3×3 silhouette dilation; subtitles/overlays and dynamic textures (water/rain/foliage) are masked out.
  - **Focus protection:** Rack-focus / focal-plane shifts hard-veto mid/fine detail borrowing so deliberate cinema defocus is never overwritten; matched-focus bokeh stays untouched by default (with an explicit low-band-only study mode).
  - **Chroma recovery:** Operates on the coded chroma grid (`4:2:0`, `4:2:2`, `4:4:4/RGB`) with exact chroma siting, 4:4:4 anchor accumulation, and carried CfL applied strictly to our prediction's luma (never double-counting luma-only coded residuals), preserving BTB/WTW on RGB delivery and falling back visibly to coarse-band chroma when the coded grid is unidentified.
  - **Real-time scheduling & graceful degradation:** Runs as a `Slice`-class post-present job (never `Fill`, never shed by the optional-fill regulator), prioritizes its 2-frame look-ahead floor over render-ahead credit at 250 ms, adapts cleanly across look-ahead leads `E = 0..6`, steps down **before** de-judder extras under GPU load while preserving quantization-cell integrity at every tier (never using L3 clamp for budget), and carries the prior delta along motion with linear decay for at most 2 late frames before returning cleanly to passthrough.
  - **Downstream dither synergy:** Preserves a physics-derived bit-depth floor on narrower output links (`10->8` / `12->8` bit) even after TD cleans the stream, prevents above-black limited-range pixels (`>= 16` at 8-bit, `>= 64` at 10-bit) from dithering into BTB while leaving true BTB pixels untouched, and visibly syncs Sigma-Delta's TD product readbacks.
  - **Strict default-OFF & Original pathway:** `temporal_detail = off` by default, exposed on the VP-V master Config page (`VP-V > Repair > Main > Temporal detail`), and forced off in the Original pathway (`Original == Bill` bit-exact).

---

## Stage-by-Stage File & Test Inventory (`TD-S0..TD-S7`)

| Stage | Scope | Production Files | Test Suites |
|---|---|---|---|
| **TD-S0** | Config knobs, `Reference`/`RealTime` shortcut ledger (`SC-01..SC-04`), visible `StandInRegistry`, W1–W4 & speed-bin metrics harness | `v2/TdV2Config.h`, `v2/TdV2StandIns.h`, `v2/TdV2Metrics.h` | `v2/tests/TdV2ConfigMetricsTests.cpp` (7/7) |
| **TD-S1** | `refs/111` closed-form `w* = C^-1 c`, FP32 shifted-origin moments, convex clip, 3σ Huber taper, blind `A1`/`A2`/`A3`, `STRICT` & `CELL` (8×8 DCT + transform-unknown bound) | `v2/TdV2BlendMath.h` | `v2/tests/TdV2BlendMathTests.cpp` (8/8) |
| **TD-S2** | Picture-only `P15`/`P15c` frame-role detector (lead-2 & causal provisional, 3:2 pulldown exact-copy exclusion, cut/fade reset) + visible `P16` block-grid stand-in | `v2/TdV2FrameRole.h` | `v2/tests/TdV2FrameRoleTests.cpp` (9/9) |
| **TD-S3** | Full CPU reference dual-source fusion engine, shared `VpComposed::SampleBicubic` + `U2 BandFromBase`, camera-relative fast-object gate, noise-debiased focus guard, 4-layer grain preservation, `U19 TdMapsView` | `v2/TdV2ReferenceEngine.h` | `v2/tests/TdV2ReferenceEngineTests.cpp` (12/12) |
| **TD-S4** | Coded-grid chroma recovery (`4:2:0`/`4:2:2`/`4:4:4/RGB`), `ChromaSiting`, 4:4:4 anchor canvas, carried CfL (`H5`), Q5 grain refinements (0.5-px pan projection check & zero-energy dark block filter) | `v2/TdV2Chroma.h`, `v2/TdV2GrainQ5.h` | `v2/tests/TdV2ChromaTests.cpp` (9/9) |
| **TD-S5 / TD-S5-OPT** | Fused GLSL compute passes + libplacebo D3D11 WARP GPU engine (`ScratchArena` `kUserTemporalDetail`, CPU-vs-GPU parity, RGB/YUV chroma apply, NaN/Inf fail-closed) + 7 zero-fidelity-loss GPU speedups (`RealTime` compact bands, zero-copy `ProcessFrameGpu`, cached chroma reuse, Chan–Pebay/Welford moments, per-corner Huber bilinear weighting for subtle face micro-motion) | `v2/TdV2Shaders.h`, `v2/TdV2Gpu.h`, `v2/TdV2ReferenceEngine.h`, `v2/TdV2Chroma.h` | `v2/tests/TdV2GpuWarpTests.cpp` (9/9 real D3D11 WARP) |
| **TD-S6** | `SpareClass::Slice` work (`TdV2SliceWork`), Q6 queue priority, lead `E = 0..6` ladder, step-down tier ladder + `OverloadArbiter` (`TdV2TierController`), Q7a 2-frame late-carry controller | `v2/TdV2Scheduler.h`, `v2/TdV2StandIns.h` | `v2/tests/TdV2SchedulerAndConfigTests.cpp` (6/6, shared with TD-S7) |
| **TD-S7** | Runtime settings parser, Q8a dependency auto-enable, live `TdProducts::Snapshot`, Ctrl+I status, VP-V master Config page (`VP-V > Repair > Main > Temporal detail`), `CoreSegments.inl` `SD SHOULD-5` sync, Q9 `DitherNeedSeam` & `SigmaDeltaDomain` + GPU shader parity | `v2/TdV2Settings.h`, `modules/control/config/{VpvRendererFields.h,VpvSchemaKeys.h,VpvRendererSettings.cpp,README.md}`, `modules/control/config/config_pages/{VpvOrganisation.h,VpvSection.cpp,VpvRenderingDefaults.h}`, `modules/repair/temporal_detail/{README.md,MANIFEST.json}`, `docs/settings/MASTER-SETTINGS.md`, `segments/CoreSegments.inl`, `modules/output/dither/{DitherNeedSeam.h,SigmaDeltaDomain.h}`, `shaders/Temporal sigma-delta.glsl` | `v2/tests/TdV2SchedulerAndConfigTests.cpp` (6/6) + `modules/output/dither/tests/DitherNeedSeamTests.cpp` (8/8) |

---

## Final Verification & `TD-S5-OPT` Performance / Parity Results

- **Full Sandboxed MSVC `/std:c++17 /W4 /WX` + Real D3D11 WARP GPU Test Run:** **68/68 PASSED, 0 warnings, 0 errors**
  - **TD v2 (7 suites): 60/60 PASSED** (`ConfigMetrics` 7, `BlendMath` 8, `FrameRole` 9, `ReferenceEngine` 12, `Chroma` 9, `GpuWarp` real D3D11 WARP 9, `SchedulerAndConfig` 6)
  - **Downstream regression check (`DitherNeedSeamTests`): 8/8 PASSED** (confirming Owner Q9a `BitDepthFloor` preserves 100% of existing `DitherNeedSeam` contracts)
- **`TD-S5-OPT` Zero-Fidelity-Loss GPU Speedup & Parity Highlights:**
  - **7 GPU speedups** integrated (`RealTime` compact band storage, zero-copy `ProcessFrameGpu`, cached chroma reuse, fused compute passes, Chan–Pebay / Welford moments, per-corner Huber bilinear weighting, M2 RGB NaN closure).
  - **3.21x 4K VRAM cut** on the `RealTime` compact band path while preserving full BTB/WTW range, film grain, and focus protection.
  - **CPU-vs-GPU WARP parity:** max abs diff **`0.000133`** code (well within `1e-3` 10-bit code parity target).
  - **Subtle face micro-motion protection:** per-corner Huber bilinear weighting preserves subtle facial micro-motion without wax smearing on both CPU (`SubtleFaceMicroMotionPreservedWithoutWaxSmearing`) and real D3D11 WARP GPU (`GpuSubtleFaceMicroMotionMatchesCpuWithoutWaxSmearing`).
- **Compliance & Guardrails:** `gen_control_config_readme.py --check`, all 10 `run_compliance.py` checks (`hook_gating`, `shared_units`, `master_settings`, `config_placement`, `module_manifests`, `private_copies`, `motion_bypass`, `test_categories`, `N2_precision`, `N4_general`), and `check_test_tamper.py --strict --range bfd055d1f..HEAD` all pass with **EXIT 0**.

---

## Owner Q1–Q9 Traceability

| Owner Q | Decision Implemented | Primary Code Location |
|---|---|---|
| **Q1** | Both `CELL` (default) and `STRICT` residual modes built; `A3 = min(A1, A2)` default (`A1`/`A2` selectable) | `TdV2Config.h`, `TdV2BlendMath.h`, `TdV2Settings.h` |
| **Q2** | Pass-1 chroma recovery on coded grid (`4:2:0`/`4:2:2`/`4:4:4/RGB` + coarse-band fallback) while fast objects/overlays stay masked | `TdV2Chroma.h`, `TdV2ReferenceEngine.h`, `TdV2Gpu.h` |
| **Q3** | Analytic `A3` primary against `MFonly_lscv_orw` metrics harness; optional `K5` weights-only net deferred to `TD-S8` empirical gate | `TdV2Metrics.h`, `TdV2BlendMath.h` |
| **Q4** | Hard rack-focus veto on `M`/`H`; `BokehMode::Untouched` default with `BokehMode::LowBandCellOnly` study knob | `TdV2Config.h`, `TdV2ReferenceEngine.h` |
| **Q5** | (a) 0.5-px sub-pixel pan bias check + projection form; (b) zero-energy dark flat block exclusion by energy (never code value, preserving BTB/WTW) | `TdV2GrainQ5.h`, `TdV2StandIns.h` |
| **Q6** | (a) Keep `250 ms` default queue budget (`kDefaultQueueBudgetMs`); (b) TD look-ahead floor (`LaneAhead::kDepthMin = 2`) visibly outranks render-ahead credit | `TdV2Scheduler.h` (`EvaluateQueuePriority`, `EvaluateLeadLadder`) |
| **Q7** | (a) Carry previous delta along motion for at most 2 late frames (`2/3`, `1/3`, `0` expired) inside quantization bounds, never L3 clamp; (b) `OverloadArbiter` steps TD down (`3->2->1->0`) before de-judder extras | `TdV2Scheduler.h` (`TdV2LateCarryController`, `TdV2TierController`) |
| **Q8** | (a) `temporal_detail != off` visibly auto-enables analysis lane + motion/camera group (default OFF; Original forces OFF); (b) laptop native 4K (`width >= 3840`) starts at `ReducedAnchor` visibly; (c)–(e) shortcut ledger & Config placement | `TdV2Settings.h` (`ResolveTdDependencies`), `TdV2Scheduler.h` (`SelectInitialTier`), `VpvOrganisation.h` |
| **Q9** | (a) `DitherNeedSeam::BitDepthFloor` (`1 - 2^-(sourceBits - colorBits)`) retains link dithering floor on narrower links with visible status/counter; (b) `SigmaDeltaDomain::QuantiseStep` AND `shaders/Temporal sigma-delta.glsl` floor above-black limited pixels at `16`/`64` while keeping true BTB untouched (CPU/GPU parity) | `DitherNeedSeam.h`, `SigmaDeltaDomain.h`, `shaders/Temporal sigma-delta.glsl`, `CoreSegments.inl` (`TdProducts::SyncRead`) |

---

## 1-Line Swap Stand-In Table (Rule 4 — Zero Silent Fallbacks)

Every missing or Wave 2–6 dependency is registered in `TdV2::StandInRegistry` (`v2/TdV2StandIns.h`) with a unique `FallbackCatalog`-style key, plain-English Ctrl+I status text, and exact swap target:

| `StandInKind` | Catalog Key | Visible Status Text | 1-Line Swap Target |
|---|---|---|---|
| `P10VariantRegistration` | `td.standin.p10_reg` | `P10 variant: td-standin-reg` | P10 variant owner registration (W2–6) |
| `P10gGrainSeparator` | `td.standin.p10g` | `P10g: single-frame stand-in (B 8+4 / H2 GPU pending)` | `kP10gGrainLevel` hybrid separator (`B 8+4 + H2`) |
| `GrainGuardW4c` | `td.standin.grain_guard` | `grain_guard: W4c port stand-in` | `repair/grain_split/GrainGuard.h` (`kUnitVersion = 1`) |
| `BandMemoW2` | `td.standin.band_memo` | `band_memo: BandFromBase stand-in` | `kBandMemo` (id 30) |
| `ComposedVectorsGpu` | `td.standin.composed_vectors` | `composed_vectors: TD U14 stand-in` | `kComposedVectors` (id 34) |
| `P3bBackwardMotion` | `td.standin.p3b` | `motion: P3b stand-in from P2/P3` | `P3b` backward motion producer |
| `P3rRefinedMotion` | `td.standin.p3r` | `motion: refinement not bound` | `P3r FieldRefineProducer` |
| `LongPairDrift` | `td.standin.long_pair` | `drift: reduced (seeded search, 1-session long pair pending)` | 1-session hinted NVOF long pair |
| `P13SourceRing` | `td.standin.p13_ring` | `scheduler: render-scope stand-in (lane/P13 off)` | `P13 SourceRingFeed` + `SpareHost` |
| `P15FramePattern` | `td.standin.p15` | `frame pattern: stand-in (CPU tile P16)` | `P15 QualityPattern` live GPU producer |
| `P16BlockGrid` | `td.standin.p16_grid` | `P16 grid: TD stand-in (exact-copy + step map)` | `kCodingEvidence` GPU 4-ch × 2-tex grid |
| `FocusMapK32` | `td.standin.focus_map` | `focus_map: TD E2/E3 stand-in` | `kFocusMap` (id 32) |
| `TdCgChromaGrid` | `td.standin.td_cg` | `chroma: coarse band only (TD-CG pending)` | `TD-CG P14g` coded chroma grid |
| `TransformUnknownBound` | `td.fallback.transform_unknown` | `transform unknown (pixel-domain band bound)` | AVC/HEVC identified transform cell |
| `LowConfidenceL3` | `td.fallback.low_conf_l3` | `L3 low-confidence coarse carry` | Full L2/L1 high-confidence path |
| `NonFiniteClosure` | `td.fallback.non_finite` | `non-finite input closed to zero` | Finite upstream evidence |
| `LateCarryActive` | `td.sched.late_carry` | `temporal detail: lane late (carried)` | On-time TD precompute slice |
| `LateCarryExpired` | `td.sched.late_expired` | `temporal detail: lane late (carry expired, passthrough)` | On-time TD precompute slice |
| `TierSteppedDown` | `td.sched.tier_stepdown` | `temporal detail: tier stepped down under GPU load` | Full (tier 3) TD budget |
| `LeadReduced` | `td.sched.lead_reduced` | `temporal detail: reduced look-ahead lead` | Full `E >= 4` look-ahead lead |

*(Additional diagnostic tokens: `StepPriorDefault`, `SingleRefA1ToA2Fallback`, `MotionFieldStandIn`, `SingularSolveClosure`, `BorderBilinearFallback`, `ChromaCflFallback`, `ZeroEnergyDarkBlockExcluded`, `GpuScratchShortage`, `GpuDispatchFailed`.)*

---

## Wave 2–6 Collision & Workspace Hygiene Inventory

Verified against `bfd055d1f` (W1 base) and Wave 2–6 hot files:

- **Bill upstream files edited:** **0** (100% of production changes live under `src/VP-V/`, VP-V's `shaders/Temporal sigma-delta.glsl`, regenerated `docs/settings/MASTER-SETTINGS.md`, and `reviews/`).
- **`src/VP-V/core/Products.h` edits:** **0** (all TD product IDs `kTdBlocks..kTdTrailSummary` already exist at W1).
- **Legacy v1 `temporal_detail/*.h/.cpp` edits:** **0** (all new v2 engine code is isolated in `src/VP-V/modules/repair/temporal_detail/v2/`).
- **`src/VP-V/modules/repair/grain_split/*` edits:** **0** (TD v2 consumes `U2 BandFromBase` directly and isolates Wave 4c `GrainGuard` behind `TdGuardPort`).
- **Minimal additive integration points outside `v2/`:**
  - `modules/control/config/{VpvRendererFields.h,VpvSchemaKeys.h,VpvRendererSettings.cpp,README.md}` and `modules/control/config/config_pages/{VpvOrganisation.h,VpvSection.cpp,VpvRenderingDefaults.h}` (additive rows for `temporal_detail`, `td_mode`, `td_overlay`).
  - `modules/repair/temporal_detail/{README.md,MANIFEST.json}` + regenerated `docs/settings/MASTER-SETTINGS.md`.
  - `segments/CoreSegments.inl` (unconditional per-frame `TdProducts::SyncRead` call for `SD SHOULD-5`).
  - `modules/output/dither/{DitherNeedSeam.h,SigmaDeltaDomain.h}` and `shaders/Temporal sigma-delta.glsl` (Owner Q9a/Q9b additive fields and CPU/GPU-matched black-floor protection, 100% backward-compatible with default-initialized callers).
