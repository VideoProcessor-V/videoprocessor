# M2 steps 4/5/8 + WP-13: dither/output glue out of the renderer host (call-site patch)

Status: **DRAFT patch, not applied** (2026-10-25, branch `feat/m2-output`). Integrator-only: the
renderer change is `m2-output-callsites.patch`; this branch commits only VP-V files, tests and
project entries. VP-ONLY (nothing here is upstreamable).

Base: `perf/byte-identical` 29c3843d + `chore/early-migrations` ac49a1de (merge d20bc45c), then the
three module moves (0aaaf2e5 step 4, 2c393254 step 5, 1f3d6fbc step 8). `git apply --check --cached
docs/design/m2-output-callsites.patch` is clean against this branch. To be rebased onto
`integ/batch1` when its hash arrives (see "Rebase notes").

## What moves out of `LibplaceboVideoRenderer.cpp` (RenderLocked and two helpers)

| Host code today (29c3843d lines) | Now | Host after |
|---|---|---|
| Stage 8-2 / 8b adaptive blend registration, bitrate weighting, residual motion, grain field, SSIM check, stats tick (24335-24522, ~190 lines) | `AdaptiveDownscaleStage::Stage::Register` (`src/VP-V/modules/scaling/output/AdaptiveDownscaleStage.*`) | one call plus the settings/host-read mapping (~45 lines; the settings struct is host-internal) |
| Stage 8-1 light domain on `frameParams` (24821-24843) | `AdaptiveDownscaleStage::Stage::ApplyLight` | one call |
| Stage 10 extended-range decision, relabel, hook placement, logs (24690-24726) | `ExtendedRangeStage::Stage::Prepare` (`src/VP-V/modules/colour/extended_range/ExtendedRangeStage.*`) | frame facts + one call |
| ER hook-failure scan after the render (25069-25073) | `ExtendedRangeStage::Stage::NoteDisabledHooks` | one call |
| Stage 6d rendered-picture cache: disarm, extras geometry (rounded crops, NLS map, subtitle holds), arm + hook, log (24727-24785) | `DejudderCacheStage::Stage::Register` (`src/VP-V/modules/motion/consumers/DejudderCacheStage.*`) | frame facts + one call |
| Sigma-delta vector helpers (`SigmaDeltaVectorsSupported`, `SetSigmaDeltaVectors`, `SetSigmaDeltaPixelVectors`, 5278-5311) and the next-frame `NextVectorGuard` (24847-24880) | `src/VP-V/modules/output/dither/SigmaDeltaVectors.h` (header-only; the legacy names stay as inline wrappers so the de-judder extras code is untouched) | guard object + `Apply` |
| Impl state: `adaptiveUpscaleThisFrame`, `adaptiveUpscaleRatioLogged`, `adaptiveMapNlsLogged`, `adaptiveStatsTick`, `downscaleLightLogged`, `dejudderWarpStageLogged`, `extendedRangeLogged/Described/HookFailed/MixLogged` | owned by the stages | `extendedRange`, `adaptiveStage`, `dejudderCacheStage` members; `extendedRangeHooks` stays as a reference to the stage's hooks (no edit at the crossfade's late restore or the GPU teardown) |

Net: the host file loses 375 lines and gains 93 (9 hunks). Shared host state that other host code
still reads (`bitrateWeightScale`, `adaptiveGrainMapWanted`, `adaptiveNoSsimLogged`,
`dejudderGeometry(Valid)`) stays in `Impl` and is passed by reference, so the status line and the
de-judder's grain-map request are not edited.

Not moved here (owned elsewhere): the sigma-delta code-domain step, dither-last reorder, occlusion
mask and encoding repair (S3, `feat/segment-runner`); the `static_refresh` loop (adjacent to the
segments patch context, left for S3); the AD status line in `PublishCustomShaderStatus` (status
text, WP-11); the extras' vector patching in `PresentDejudderExtras` (WP-7 / M2 step 17); NLS and
`NlsHookRecovery` (Bill's). The now-unused `#include <vprenderer/DownscaleLight.h>` in the host is
left for the M2 step-0 include sweep (it sits in the segments patch's context).

## Why it is byte-identical (two-commit rule: this is commit 1, behaviour-free)

- Every statement moved verbatim, in order; host reads happen at the same points (the stage asks
  through `Host` callbacks exactly where the code asked: the source monitor twice, the NLS map and
  the SSIM check only on their branches, `AdaptiveFallbackFramePeriod` only without a source rate).
  The de-judder reads (`DejudderLiveFieldFor`, `VpPhaseDrawn`, `Field`, `FrameTrust`, `GrainError`)
  are const getters, gathered once per branch instead of per condition.
- The de-judder cache's NLS map uses `KnownNlsMapping()`, which is the same code as the inline
  block it replaces (bundled NLS / NLS+ map, otherwise an invalid map; both give `mapped = false`).
- Same hook objects, same positions, same parameters, same log text and once-only latches.

CPU tests (`src/VP-V/tests/OutputGlueStagesTests.cpp`, class `OutputGlueStagesTests`):
`ExtendedRangeStageIsTodaysDecisionRelabelAndHookList` compares the stage against a copy of today's
code for 5 sources x 4 targets x 32 flag combinations (relabel, colour, hook signatures, reason);
hook-failure latch and the BTB/WTW fallback decision; adaptive off / SSIM missing / downscale /
upscale hook order, shared state, status tick, the two monitor reads, the light domain; the cache's
arm/geometry/holds/NLS/interpolating cases; the vector guard.

## GPU byte identity (written, not run)

`src/VP-V/tests/OutputGlueStagesGpuTests.cpp` (Category `GpuHardware`, outside the filtered CPU
suite; WARP by default, `VP_TEST_GPU=hardware` for the NVIDIA adapter):

- `ExtendedRangeStageRendersTodaysCodes`: 4 colour cases x more-cases on/off, today's hook list vs
  the stage's, **0 codes** difference; BTB (code 4) stays below black and WTW (1019) above white.
- `AdaptiveUpscaleStageRendersTodaysCodes`: 3 consecutive frames of the 2x upscale form, **0 codes**.

Big test (WP-20) with the patch applied: frozen-b3base byte identity, BTB/WTW probes, whole-chain
matrix, as WP-13's acceptance says.

## Compatibility with the other drafted host patches

- `segments-callsites.patch` (feat/segment-runner 8b4806a0): its renderer hunks still apply on top
  of this patch (checked with `git apply --check`); no hunk overlaps.
- `silent-fallbacks-callsites.patch` (WP-4) adds `SyncFallback(FallbackCatalog::BtbWtw(...))` after
  the extended-range block. After the rebase that line becomes
  `SyncFallback(extendedRange.Fallback(activeSettings.preserveExtendedRange));`
  (`ExtendedRangeStage::Stage::Fallback` is the same mapping, CPU-tested).
- `efficiency-callsites.patch` (WP-5) adds the ER idle texture release and the two-pass-carry entry
  inside the extended-range block: on the rebase they go into `ExtendedRangeStage::Prepare`
  (hooks owned by the stage; `CarryHooks()` gives the efficiency code its `HoldsTextures` /
  `Release` / `CarryPasses`). Its other hunks (8844 teardown etc.) are untouched by this patch.

## Rebase notes (onto integ/batch1)

1. Rebase the branch; regenerate the patch against the new base (re-apply the edits above to the
   host, then `git diff > docs/design/m2-output-callsites.patch`); `git apply --check --cached`.
2. Fold WP-4's BTB/WTW line and WP-5's ER idle/two-pass lines as described above.
3. If the segments patch has landed, `OutputSegments.cpp` may switch to the
   `<VP-V/modules/output/dither/...>` includes (the forwarders keep it building until step 0).
