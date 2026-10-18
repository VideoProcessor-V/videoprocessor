# No fixed constants: batches B3-B7 (agent report, 2026-10-18)

Branch `feat/no-fixed-constants-2` from local `feat/no-fixed-constants` at b1f02c4c (B1/B2), worktree
D:\codex\videoprocessor\fixedconst2. Local only (no remote calls), no GPU runs. Inputs:
docs/VP-fixed-constants-inventory-2026-10-18.md (deferred list), docs/agent-reports/no-fixed-constants.md.

## One rule for every conversion (src/VideoProcessor-Lib/GeneralRate.h)

- **Frames -> time with a frame floor** (`GeneralRate::FramesWithFloor`): a count tuned in frames keeps
  its value at and below 60 Hz and holds the same time (count / 60 s) above it, rounded up, with a
  0.1 % measurement tolerance (a 16666 us period reads 60.002 Hz and does not add a frame). So 23.976 ...
  60 Hz are numerically unchanged and 100-240 Hz keep the time instead of a window a quarter as long.
  60 Hz is the highest standard rate the counts were tuned at, not a rate list.
- **Per-frame weights** follow the same rule (`PerFrameWeight`: unchanged <= 60 Hz, same time constant above).
- **Pixels -> height with a floor** (`GeneralScale::AboveReference`): unchanged up to 2160 lines, by
  height above (the rule GrainPreserve::GrainScale already used). 1080 and 2160 unchanged.
- **Unmeasured-rate fallbacks** are kept and logged once each time they start being used
  (`FallbackNotice`, `GeneralRateLog.h::NoteFallback`: "Rate fallback: <site> has no measured rate; assuming N Hz").
- **Log throttles**: `LogInterval` (frames -> the seconds the count lasted at 60 Hz: 120 -> 2 s, 600 -> 10 s, 2400 -> 40 s).

Switch: every behaviour change is behind `general_rate_fixes` (B1/B2, default on). The renderer reads its
profile setting; code without a renderer profile (DirectShow pin, GUI queue target, refresh-key lookups)
reads a process-wide copy the GUI publishes from each resolved profile snapshot
(`UnifiedProfileRuntime::Snapshot::generalRateFixes` -> `GeneralRate::SetProcessEnabled`). Log-only
changes are unswitched.

## Converted

| Sweep # | What | Where | Default at 24/60 Hz, 1080/2160 |
|---|---|---|---|
| 6 (B3) | Time-target ceiling `MaxTargetFrames 16` -> `MaxFramesForLatency` (16 frames <= 60 Hz, 267 ms above: 32 @120, 64 @240); capacity clamp unchanged | QueueLatency.h:22-41, :74, :88; VideoProcessorDlg.cpp (ResolveTargetFrames call, SetProcessEnabled) | identical |
| 7 (B3) | Motion-ahead depth -> `MotionAhead::DepthAt` (4 <= 60 Hz, 66.7 ms above: 8 @120, 16 @240, cap 16); the queue minimum follows (UnifiedProfileRuntime); producer pending jobs follow the depth (`PendingJobsFor`: 6 at depth 4) | MotionAhead.h:21-35, :102; UnifiedProfileRuntime.cpp:1224-1237; MotionAheadProducer.h/.cpp (SetDepth); LibplaceboVideoRenderer.cpp:20500-20509 | identical (depth 4, queue 5, 6 jobs) |
| 14 (B3) | DirectShow startup buffering: no 30 fps step -> `max(nominal+1, min(5, ceil(208.5 ms x fps)))` | DirectShowRatePolicy.h:25; CBufferedLiveSourceVideoOutputPin.cpp:7674 | identical with the default queue (5 at every rate); low latency bounded by capacity 1 as before |
| 15 (B3) | Delivery hold 50 ms -> max(50 ms, 1.15 x measured period, <= 1 s) | DirectShowRatePolicy.h:52; CBufferedLiveSourceVideoOutputPin.cpp:260 | identical from 23 fps up (50 ms) |
| 16 (B3) | Convergence paced-plateau evidence (warm-up 12, paced 6) -> FramesWithFloor; priming depth kept (it is the downstream queue's depth in frames) | LiveEpochConvergenceController.h/.cpp:323-334 (input `generalRates`) | identical |
| 9 (B4) | Camera window step at the half-search threshold (~1527 lines) -> continuous `max(6, 8 x height / 2160)` | DejudderPolicy.h:355; DejudderWarp.cpp (camera_window) | identical at 1080 (6), 1440 (6), 2160 (8), 4320 (16) |
| 10 (B4) | `consistencyPixels` (and the NVOF FB tolerance) by working height above 2160 | DejudderWarp.h:533 `ConsistencyPixels()`; DejudderWarp.cpp:3311, :3721 | identical <= 2160 |
| 11 (B4) | Engine global vote range +-40 texels by field height above 2160 lines (`FlowVoteRange`) | MotionAheadProducer.h:78-90; MotionAheadProducer.cpp; DejudderWarp.cpp FlowGlobal | identical <= 2160 |
| 18 (B6) | Debander hold `holdAlpha 0.5` per frame -> PerFrameWeight | LibplaceboVideoRenderer.cpp:20679 | identical (0.5) |
| 22 (B7) | One refresh-key convention (RefreshKey.h): keys are rates (60, 59.94, 60000/1001); exact (400 ppm) wins, then an integer key's legacy band, then nearest within 0.5 Hz; `LegacyKey` is the one band function. Migration: saved integer keys that stand for 1000/1001 rates (23, 29, 47, 59, 71, 89, 119, 143, 239: K+1 a multiple of the NTSC 24/30 bases) become 23.976, 29.97, ... - in memory at load (ConfigFile) and in the file by the Config editor (migration backup on save) | RefreshKey.h:85, :147, :187; ConfigFile.cpp:401; PPMCorrectionLoader.cpp:167, :243; VideoProcessorDlg.cpp:2475, :5332; ConfigEditorWindow.cpp:2420-2450; MadVRShaderLoader.cpp:2488; LibplaceboVideoRenderer.cpp:4109 | identical for every standard rate (tests); the fix: a measured 59.9997 Hz now finds key 60 (was band 59) |
| 25 (B7) | Fallback log at 17 sites (DS pin x2, renderer x6, ActivePictureTransitionModel, AlphaCadenceCorrectionPolicy, AlphaSourceCropPolicy x2, BufferedPictureExpansion, AdaptiveDownscale, SourceResolutionMonitor, CR210 DV formatter, pulldown, render-stall threshold) | see `NoteFallback` | values unchanged |
| throttles (B7) | 10 renderer, 4 DS pin, 1 DS renderer frame-count throttles -> seconds | LibplaceboVideoRenderer.cpp, CBufferedLiveSourceVideoOutputPin.cpp, DirectShowVideoRenderer.cpp | log only |

Config: the "General frame-rate rules" tooltip lists the new behaviours.

## Already converted / kept (checked)

- #8 EarlyUpload slots and #13 cadence-correction timing (B5): converted by the timing merge.
- #17 grain: the low-band sigma, split sigma, tensor block and along step already use GrainScale
  (flat <= 2160, by height above; hunt 2 C15). Left: the 16x16 guard workgroup and `kGainMinPixels`
  (a sample count, not a distance) - a workgroup change needs a GPU run.
- #19 holdHysteresis: already per frame at the 23.976 reference (Scaled()); #20 D2 per source frame (owner).
- #21 sigma-delta search (+-64 output px, 16x16 workgroups): the shader's pyramid structure; identical
  <= 2160 output under the height rule, scaling needs a GPU-verified shader change. Not done.
- #24 subtitle `offset_pixels` / `gray_blur_px`: feed ~8 subtitle paths; owner rates subtitle fidelity
  low; under the height rule nothing changes <= 2160. Not done.
- Event-count throttles (`++failures == 1 || % 2400`, capture drops `% 300`) are event counts, not frames: kept.
- Seeds of measured averages (LedDriveModule 1000/60 EMA seed, LedController 1000/24) and policy-header
  defaults read through logged call sites: kept.

## #5 proposal (not implemented: needs a GUI switch and the owner's OK)

`RankDisplayRefreshModesForInput` (DisplayRefreshRatePolicy.cpp:163) appends doubled-rate modes only
for 24.1-31 Hz input (and interlaced). Proposal: after the native group, append groups for the whole
multiples N x input (N = 2, 3, ... up to the display's maximum), smallest N first, each marked with
its multiple - so 48p gets 96/144, 15p gets 30/45/60, 23.976 gets 47.952/71.928/119.88 when no
native mode exists, 50p gets 100. Behind a new Display switch `refresh_multiple_ranking` (Config:
Display > "Prefer whole multiples of the source rate"), default off until the owner approves; off =
today's ranking byte for byte. Tests: ranking tables for 15/23.976/24/25/29.97/30/48/50/59.94/60/100/120
against mode lists with and without native modes. About 1-1.5 h agent time.

## Tests

- GeneralRateMatrixTests.cpp extended (41 new methods in 11 classes; 69 methods in the file; written by a
  sonnet sub-agent from my brief, compile/expectation fixes by me): FramesWithFloor, PerFrameWeight,
  LogInterval/FallbackNotice/process flag, QueueLatency, MotionAhead depth/queue/jobs, DirectShow startup
  buffering and hold, convergence evidence, camera window, height scale and flow vote range (FlowGlobal
  identical on/off), RefreshKey (band key, parse, migration table, Find equivalence before/after
  migration, Apply), PPM end to end. Rates 23.976-240 incl. 1000/1001, 71.928, 90, 143.856, 165, 200
  and odd 15, 20, 35.5, 61, 62.5, 75, 59.999, 60.002 (16666 us); heights 480-4320 plus DCI, scope,
  anamorphic rasters.
- **Proof of identical defaults**: every converted value equals the legacy value at every rate <= 60 Hz
  (incl. 23.976, 24, 59.94, 60 and the 16666/16667 us periods) and with the switch off; every
  height-scaled value equals legacy at 1080 and 2160 (camera window also at 1440 and 4320); the DirectShow
  startup target is 5 at every rate with the default queue; the refresh-key lookup picks the same entry
  before and after migration at every standard rate.
- Deliberate differences (asserted): >60 Hz windows keep their time; DirectShow startup with a
  non-default queue (nominal 0 at 60 fps: 5 vs 3 - a queue_size 1 low-latency profile is still clamped to
  its capacity 1; nominal >= 5 at 24 fps: +1; nominal 0 at 15 fps: 4 vs 5); a measured 59.999 Hz finds
  key 60 (legacy: band 59).
- Updated: VectorStoreTests `DepthFollowsTheSettingTheQueueAndLowLatency` (Decide now clamps to
  kMaxGeneralDepth; DepthAt(9, 24/60) = 4 asserted).
- Results (x64 Release, CPU only, single-threaded vstest, TRX, GpuHardware/GpuOnScreen excluded):
  - Full suite: 3413 run, 3409 passed, 4 failed. Fixed: the Decide clamp expectation (above). Not caused
    by this change: `DeckLinkConvertersSustained4K60PerformanceComparison` and
    `CDeckLinkRGBToP010VideoFrameFormatter4KSmokeTest` (CPU-speed assertions, also failing in the B1/B2
    run under build load) and `ConcurrentInvalidateAndCheckStress` (MotionValidity thread stress,
    untouched; passed 3/3 alone).
  - Targeted re-run after the fixes (GeneralRateMatrix, MotionAhead, VectorStore, MotionValidity,
    QueueLatency, ConfigFile, ConfigEditorCore, QueueTimingFixes, RendererCaptureHarness, LiveEpoch): 370/370 passed.
  - Config tests (off screen, 1 shard): 143 passed, 0 failed, 1 skipped (needs a real window); gate passed.
- Not run: GPU tests (none allowed); a chain-gate pass and a real-clip check at 120p+ are due before any deploy.

## Merge notes

LibplaceboVideoRenderer.cpp edits are one-to-three-line changes marked "fixed-constant batch B3/B4/B6/B7
2026-10-18" (includes, the refresh-command key, 10 log throttles, 5 fallback notices, the matcher
settings' generalRates, the motion-ahead depth/producer, the deband hold). The timing/queue code is
touched only at QueueLatency's ceiling, the DirectShow startup/hold helpers and the convergence evidence
counts, each a separate pure function with the legacy path kept.
