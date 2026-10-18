# Bug hunt 3 (hunter A): timing fixes

Branch `fix/h3-timing` from `integ/2026-10-17` (17e534c3). Findings: `D:/codex/hunt3-scratch/a/findings.md`.
CPU tests in `src/VideoProcessor-Test/H3TimingTests.cpp` (class `H3TimingTests`); GPU tests for Claude in
`RendererCaptureHarnessTests.cpp`. No GPU was run by the agent.

## Fixes

| # | Fix | Test |
|---|-----|------|
| F1 (M) | The engine global vector is read at `DejudderPolicy::EngineGlobalSourcePixels` = (8 << `SearchHalvings()`) x \|g\| (DejudderPolicy.h; LibplaceboVideoRenderer.cpp `RunDejudderEstimate`, latch `SetMotion` and intent `NoteVectors`). 4K was read at half speed, 8K at a quarter. | `F1EngineGlobalVectorScalesWithTheSearchHalvings`: 1080/2160/4320, 0.6 h/s pan at 24 fps; latch heights/s = 0.6 (above the 0.5 fast-pan switch) and intent pan = 0.6 through the observer at every size (before: 0.6, 0.3, 0.15). |
| F4 (M) | `MotionEngineIdle::Keeper` (new header). With hiccup repair on, the engine is kept loaded and idle for every reason a gap pair can still be repaired under (Off, IneligibleCadence, VideoRateMaskOnly); released as before for low latency, interlaced, smooth motion, native RGB (the standalone repair never runs there). Low-VRAM fallback: an allocation failure (B's upload, an engine "cannot create" texture error) or DXGI budget pressure (QueryVideoMemoryInfo, polled once a second while idle, usage >= 95% of budget) releases it while idle; kept again after 30 s with usage + the engine under 85%; recreated by the next gap pair. Creation is logged ("Motion engine: created (n times ...)"). | `F4SecondGapDoesNotRecreateTheEngine`, `F4LowVideoMemoryReleasesAndRecreatesLater`; GPU: `H3IdleMotionEngineKeptAcrossHiccups`. |
| F5 (L) | A standalone gap pair on a source the de-judder is Active on keeps that frame's decision (`InterpolationTimeline::StandaloneGap`): strength = owner strength x intent scale (not 1.0), even cycle kept; the timeline draws the pair at phase x strength (`PrepareVpPhasePicture`). Extras stay off for that frame because the timeline draws its refreshes. | `F5StandaloneGapKeepsTheActiveDecision`. |
| F2 (L-M) | `QueueLookahead::LockedFilmLookaheadFloor`: an automatic look-ahead sets no locked-film floor (it follows the queue it is given); an explicit value keeps lookahead + 1. The GUI tells the renderer via the new `IVideoRenderer::SetActivePictureLookaheadAutomatic` (end of the vtable; plugin proxy forwards; `ApplyStartupActivePictureLookahead` at every Build, and on profile changes). | `F2AutomaticLookaheadKeepsTheLockedFilmScaling`: 3:2, target 5 -> 2, target 8 -> 3 with auto (before: 5 and 8); explicit 3 -> floor 4. |
| F3 (L-M) | `AlphaArrivalRateEstimator::Add` restarts when the hint moves by more than 1% (`kHintChangeShare`; a pulldown lock change), as VBlankGridRate does; a 1000/1001 step keeps the fit. | `F3ArrivalRateRecoversWithinSecondsOfALockChange`: the replica scenario; within 200 ppm 12 s after the unlock (before: about 15 min) and 12 s into the lock. |
| F6 (L) | `IntentInterpreterObserver::ObserveGrid`: contiguous only when `sequence == last + 1`. | `F6SkippedFrameIsADiscontinuity`: sequences 1,2,4,5 give the same pan as 1..5; 1,2,4 makes no cut. |
| F9 (L) | Fade thresholds in time (`MotionIntent::FadeStepCodes/FadeMinRunFrames/FadeWindowFrames`, `FadeRun`): 24 codes/s, run 0.125 s, window 0.25 s; identical at 24p (1 code, 3, 6), 48p: 0.5, 6, 12. | `F9FadeThresholdsInTime`: a 4 s 100-code fade found at 24p and 48p after the same time. |
| F10 (L) | `dejudderEstimatedPeriods` (gap pair: k + 1, `InterpolationTimeline::FieldPeriods`); texture_restore takes the field only when it spans one period (one line in the detail block). | `F10GapPairFieldIsNotOnePeriod`. |
| F8 (L) | Config queue page: a notice under Active-picture lookahead when the profile's effective value is an explicit 0 (`QueueLookahead::ExplicitZeroLookahead`, `kExplicitZeroLookaheadNotice`); follows edits; no migration. | `F8ExplicitZeroLookaheadNotice`. |
| F7 | Unchanged. Gated harness test added for Claude. | GPU: `H3LookaheadCostCropSubtitle4K`. |

## F4: VRAM held by the idle engine

From the texture sizes (P010 4:2:0 source, W x H):

- warped planes (source format, 3 B/px), B's planes (3 B/px), the interpolator's output planes (3 B/px): 9 B/px
- interpolator grain bands (interpolation_grain preserve; 4 half-size 32-bit float textures per plane, luma 1 and
  chroma 2 components): about 6 B/px
- matcher half-size r16f planes (A, B, K) and pyramids, plus the inner instance at 4K: about 1.5-2 B/px
- block fields (vec4 f16 at 1/16 x 1/16): under 0.2 B/px

About 17 B/px: **1080p about 35 MB, 4K about 141 MB**, plus the optical-flow engine's own session buffers
(grayscale inputs at the engine size, 4x4 output grid, driver internals: not measurable on CPU) and, when
motion_ahead is on, the producer's device. `MotionEngineIdle::kIdleBytesPerSourcePixel` uses this figure for the
recovery headroom check.

## GPU tests for Claude

- `H3IdleMotionEngineKeptAcrossHiccups` (GpuHardware, `VP_TEST_GPU=hardware`): 60p-class source at the monitor
  rate, `dejudder_strength: 0.5`, interpolation off, two single-frame gaps at 5.0 s and 6.5 s; asserts both gap
  pairs, exactly one "Motion engine: created", no idle low-memory release.
- `H3LookaheadCostCropSubtitle4K` (opt-in `VP_TEST_H3_LOOKAHEAD=1` and `VP_TEST_GPU=hardware`): 4K 59.94 V210
  through the CPU formatter (P010), letterboxed 2.39 with automatic crop on a 2.35 screen, classic subtitles,
  target 5; key absent (automatic, 4) vs 0; reports lookahead_avg_ms / lookahead_max_ms from
  "Alpha pre-render timing" lines. F7 measurement for the owner's laptop question.

## Notes

- Repairs for low latency, interlaced, smooth motion and native RGB were never possible (the standalone gap fill
  excludes them), so the engine is not kept there; that is the only exception to "every de-judder reason".
- F3: after a hint change the estimator reports 0 (unknown) for its 10 s minimum span, never a wrong rate.
- F6 relies on every enqueued frame taking the next sequence (repeats share their frame's), which holds in the
  enqueue path; a queue drop the walk never saw is now a discontinuity too, as it should be.

## Test runs (CPU, single-threaded, TRX)

- Before the fixes (F3 and F6 reverted, everything else built): `F3...` failed (1.43e6 ppm 12 s into the lock),
  `F6...` failed (0.592 vs 0.444 h/s across the hole); the other H3 tests use helpers that did not exist.
- Hunter filter (41 classes) + H3TimingTests, DetailPulse, DetailRestoreConfig, TextureRestore, RenderAheadPolicy,
  IntentInterpreter, SourceResolutionLatch: **309/309 passed** (`D:/codex/h3t-logs/trx1`).
- After the final edits: H3Timing, IntentInterpreter, MotionIntent*, AlphaCadenceCorrectionPolicy, ContentIntent,
  QueueLookahead, Hunt2CadenceClass*: **80/80 passed** (`D:/codex/h3t-logs/trx2`).
- Config (off screen, 1 shard, queue/look-ahead): 6/6 passed.
- test/real-footage-suite (8ceaaa33) is on an older base (167 files differ), so it was not merged in here.
