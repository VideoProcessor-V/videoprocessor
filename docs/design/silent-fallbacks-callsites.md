# Silent fallbacks: renderer call sites (hand-off for the skeleton lead)

This is the module findings 2026-10-24 "silent fallbacks" batch: part 1 item 2 and the most serious
part 2 findings. The rule is "host = call sites only":

- The decisions, the wording, the stable ids and the tests live on this branch in
  `src/VP-V/core/FallbackCatalog.h`, CPU-tested by `src/VP-V/tests/FallbackCatalogTests.cpp`.
- The renderer only passes the facts it already has. Each site becomes one `SyncFallback(...)`
  line, which engages or releases the entry and logs the heading once.

## Apply

```
git apply docs/design/silent-fallbacks-callsites.patch
```

The patch was generated against `chore/early-migrations` at the commit that adds it, whose
`LibplaceboVideoRenderer.cpp` is `feat/skeleton` `fd60b650`. `git apply --check --cached` passes
there. On a later renderer, apply with `git apply -3` or by hand: the patch consists of 23
self-contained hunks, listed below. Apply it after `chore/early-migrations` has merged, because it
needs `FallbackCatalog.h`.

## What it changes

| Finding | Site (function / anchor) | Change |
|---|---|---|
| Sigma-delta dropped (part 2 #1) | the sigma-delta code-domain loop; `CustomHookChain::Chain` | Records a domain drop. A new `Chain::QuantizerProblem()` reports a quantiser shader that did not load or was disabled. Entry: `dither` |
| BTB/WTW clipped (part 2 #2) | after the extended-range decision and the `mixClipsBtb` log | Hook failed, frame mixing, or smooth-motion BTB clip engages `BTB/WTW`. Design exclusions stay status-only |
| Re-base slot off still drives downstream scaling (part 2 #3) | after `rebaseFactor` / `rebaseFactorX` are set | `EffectiveRebaseFactors` forces both to 1 when `scaling.rebase` does not run. The failure pause engages `re-base` |
| Debander failure = no deband (part 2 #4) | where libplacebo's deband is re-enabled for temporal frames | `PlanDeband`: libplacebo's deband stands in only when VP's debander has FAILED. Entry: `deband`. The failure is sticky for the session (`Failures() > 0`). The `repair.deband` slot switched off (pipeline_slots_off, Original pathway) is a choice: no substitute and no entry, and libplacebo's deband follows the user's settings as before. Dolby Vision tunnel pictures are never substituted (see below) |
| Optical-flow failure (part 2 #5) | the de-judder motion-source block | Engine failed, or the look-ahead producer stopped. Entry: `motion` |
| Open-loop presentation (part 2 #6) | after the render-ahead display-rate choice; the de-judder timing check | Open loop, or the de-judder held for want of a display rate. Entry: `presentation`. New member `dejudderHeldNoDisplayRate` |
| Failed refresh-rate switch (part 2 #7) | `ScopedDisplayRefreshRate::Switch` | New `SetFallbackSink`. Every "retaining" exit reports `refresh rate (kept X Hz for Y Hz input: why)`. Each confirmed or applied switch releases it. Wired next to `SetEventSink` |
| SDR tone shape shader not loaded (part 2 #8) | before the SDR tone shape state log | State 3 engages `SDR tone shape` |
| Early upload unavailable (part 2 #26) | `earlyOpenFailed` | Entry: `early upload` |
| Test project | `VideoProcessor-Test.vcxproj.vcxproj` | Adds `..\VP-V\tests\FallbackCatalogTests.cpp` (NotUsing PCH, like DelayGateTests) |

## Interaction with feat/dv-l8 (deband)

feat/dv-l8 runs neither debander on Dolby Vision tunnel pictures. `PlanDeband` therefore never
substitutes libplacebo's deband on them and engages no entry. The patch passes
`renderImage.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION` as the DV-tunnel-picture fact. If dv-l8
has its own "tunnel picture" flag (the one it uses to skip the debanders), pass that flag instead
when merging, so the two rules can never disagree.

## Builders ready but not yet wired (no clean single anchor; wire with the owning change)

These are all in `FallbackCatalog.h`, tested, with their ids fixed:

- `CalibrationLutDropped`: in `RejectDisplayLutAfterRenderFailure`.
- `CalibrationPatchFullField`: in `PresentCalibrationPattern`, at the windowed-to-full-field
  fallback. Also report it to the session.
- `OutputRangeComposed`: in `EnsureAutoLimitedFallback`.
- `OutputRequestRejected`: at the initialise path that renders a rejected request on Full/sRGB.
- `GeometrySafeFit`: next to `SetShaderStatus("NLS: Safe fit (hook failure)")`.
- `SubtitleDetection`:
  - `CueDropped` at `Consume`'s "preview-unavailable" reset;
  - `JobsDropped` when `SubmitMeasurement`'s dropped counter grows;
  - `WorkerError` when the worker's error counter grows;
  - release at the next good cue.
- `OutputScaling`: when `AdaptiveDownscale::Failures()` grows, or SSimDownscaler is missing while
  `adaptive_downscale` is on.

The LED module's own entries (`LED drive`, `projector`) are merged as described in
`module-findings-2026-10-24.md` item 1.

## Verify

Run the CPU tests `FallbackCatalogTests` plus the existing renderer CPU tests.

Byte identity: no picture changes except in three places, each only while its fallback is in
force:
- the re-base factors with the slot off;
- libplacebo's deband when VP's debander has failed or its slot is off;
- the refresh sink (no picture effect).

The chain gate's byte-identity subset should stay identical with default settings.
