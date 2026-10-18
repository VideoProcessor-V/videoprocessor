# Hunt 2, fix group 5: timing and motion (2026-10-17)

Branch `fix/hunt2-timing`, worktree `D:\codex\videoprocessor\hunt2-timing`, base 1592d39d
(merge-v1.4.02 with the render-ahead fixes). Local only: GitHub was unavailable, so there was no
cloud CI. The verification below was run on this machine.

Sources: hunt reports 13-timing, 04-motion, 06-interp, 02-dv (the classifier and DV pairing
items), 15-intent C3/C4/C11, 10-layout M1 and 05-renderahead (M5-rest, L3, L5).

## Group switches (all default on, each with a Config control under "Fixes under test"; off restores the old behaviour)

| Switch | Config label | Covers |
|---|---|---|
| `queue_timing_fixes` | Queue timing fixes | 13-timing, 05 M5/L3/L5 |
| `cadence_class_fixes` | Film / video classification fixes | 06-interp, 02-dv M2/M3/M4/L6, 15-intent C4 |
| `motion_history_fixes` | Motion history fixes | 04-motion M1/M2/L1-L4, 02-dv L2 |

Each switch is wired through the settings struct, fingerprint, profile parse,
LiveProfileSettingsCompatible, settings dump, RendererProfileConfig validation, the Config
control and the defaults table.

## Fixes

Paths are relative to `src/VideoProcessor-Lib/vprenderer/` unless shown; LVR = LibplaceboVideoRenderer.cpp.

### Queue and timing (`queue_timing_fixes`, sub-agents A and A2)

**H1/H2, the top bug.** With interpolation on at 25-50 Hz, the queue dropped a frame about every 80 s.
- The backlog flush now measures the oldest frame's age beyond the target's own wait (AlphaQueuePolicy.h StaleBacklogExcessAgeMs).
- The flush uses the queue's own frame rate: the front frame's film rate while pulldown is locked (QueueFrameRateHz, LVR flush site).
- The timeline steering (TimelineQueueSteering) keeps a one-frame margin, which is no longer flushed. Its smoothing is a 0.8 s time constant.
- Simulation: 0 flushes from 23.976 to 120 Hz at ±100 ppm, and also at 3:2 (60/24 and 59.94/23.976).

**Cadence correction**
- **M1:** the capture rate is measured on QPC arrival times (AlphaCadenceCorrectionPolicy.cpp, AlphaArrivalRateEstimator).
  - Missed frames count their periods, film updates are ignored, and a break restarts the estimate.
  - The nominal fallback rate never feeds the rate filter.
  - The phase is bounded to ±2 frames.
- **M2:** warm-ups, cooldown and the rate filter are in seconds.
- **L6:** a disabled policy forgets its phase.

**Other queue items**
- **M3:** the credit-reduced target is floored at the look-ahead frames (PrefillTargetLocked).
- **L1:** an overflow drop merges the dropped frame's gap into the next frame's (MergeDroppedGap). A no-fill gap survives the drop.
- **L2:** TimingClock.cpp keeps sub-millisecond readings and returns 0 when the clock rate is 0. No switch: correctness fix.
- **L3:** the failure limit (5 s), the phase-log gate and the steering are in seconds. **kMaxSpanFrames stays a frame count: owner decision** (the tooltip promises 1-3 frames).
- **L4:** the early-upload slot count follows the queue target.
- **L5:** sourcePts is 0 when the capture rate is 0.

**Render-ahead**
- **M5:** the whole-Hz mode now ranks below the vblank cadence. In open loop, the planned refresh is refined from DWM vblank times (OpenLoopDisplayRate.h).
- **L3:** the open-loop lead is published as an estimate.
- **L5:** libplacebo's D3D11 swap_buffers (7.360.1 and 7.377) is only `Present(1,0)` plus failure and device-lost bookkeeping. It has no frame-latency wait and no counter, so the native interval presents bypass nothing. Their HRESULT failures are now reported.

**15-intent C3**
- Literal 10/240/480/500 Hz bounds now go through `DisplayRefreshBounds` (display) or the new `CaptureRateBounds` (DisplayRefreshRatePolicy.h).
- A grep test forbids the literals.
- Behaviour change: display-side checks now accept up to 500 Hz instead of 240.

### Pulldown, film clock, classification (`cadence_class_fixes`, sub-agent B)

**Pulldown and film clock (06-interp)**
- **H1:** the 15 fps film floor is exclusive with a relative 1% margin (PulldownDetector.h AboveFilmFloor).
  - The corpus INFO block is now an assertion: real footage gives 0 locks at 60 and at 59.94; with the switch off, 481/600.
- **M1:** `FilmClock::Reanchor()` runs on a re-phase. Lateness is uncertain while a re-phase is tracked.
- **L6:** the anchor is the second-largest offset, so one late frame does not step the clock.
- **M2 / C4:** `source_class=measured` classifies from the source alone, never from the display ratio (InterpolationPolicy.h).
- **M3 (D2 wired):** the queue target is scaled by new/cycle while locked (LockedFilmQueueTarget).
  - Floor: the look-ahead minimum (motion-ahead QueueFramesFor, active look-ahead + 1). At the defaults, nothing changes.
- **L1:** a toggle restart of the film clock breaks the pair.
- **L2:** a mid-lock settings change clears the re-phase state.
- **L3:** a retry unlocks only when the locked phase failed again.
- **L4:** film updates are counted apart from skipped frames.
- **L5:** film within 5% of 1:1 is left to drift correction.

**Content classifier (02-dv)**
- **M2:** carrier repeats are judged by rate, not by even holds (ContentClassifier.h).
- **M3:** at film rates, animation needs positive evidence, and static UI needs three times the still time.
- **M4:** content intent works without look-ahead frames.
- **L6:** in profiles selected by `content`, the classifier's input keys are ignored, with a validation warning.

### Motion (`motion_history_fixes`, sub-agent C)

**Behind the switch**
- **04 M1:** the matcher's history restarts on an unchained pair (DejudderPolicy::MatcherHistoryChained, DejudderWarp.cpp).
- **04 L2:** ForgetHistory also drops the strength state.
- **04 M2:** the live-field gate `dejudderLiveFieldFor` keeps A's repeat draws off the (B, C) field estimated ahead.
- **04 L1:** the ahead frame C passes B's adjacency and DV rules.
- **04 L3:** SettingsChange and MotionSourceChange are now raised.
- **04 L4:** a repeat-preserving drop marks a gap.
- **02-dv L2:** DV pairs need byte-equal DM matrices.

**No switch (correctness fixes)**
- **04 M3:** the optical-flow reach uses the scaled search range.
- **04 M4:** the sentinel SAD is 1e4.
- **04 L5:** camera window = max(6, 4·2^halvings).
- **02-dv L1:** the producer's DV kind comes from ClassifyDolbyVisionPayload.
- **10-layout M1:** on an NLS hook retry, the de-judder cache is invalidated, the geometry is dropped, the extras hold, and the occlusion mask is off.

**15-intent C11**
- Config controls added for:
  - bounded_invalid_capture_recovery;
  - the DeckLink RGB packing keys;
  - DirectShow conversion_method, min_core_count and max_core_count;
  - ppm;
  - subtitle_detection_optimization;
  - the subtitle hold and near-bar values.
- The coverage test now reads every MainConfigSchema list.
- `chroma_downsampling` gets no control. Bill's test requires it hidden (manual-only P010 option); it is allow-listed with that reason, and a control is the owner's call.

## Lead's cross-area sweep

1. **Pulldown film queue vs the backlog flush.** The flush was given the signal rate while the locked queue holds film frames, so it still flushed at target+1. Fixed: the flush uses the queue's own rate, with a 3:2 test.
2. **Locked queue target vs motion-ahead.** B's first version floored the scaled target at 2, which would have disabled motion-ahead (4 queued ahead) and the active look-ahead on pulldown content. Fixed: the floor is the look-ahead minimum.
3. **Credit floor (A, M3) composed with the locked target (B).** Both live in PrefillTargetLocked; checked consistent.
4. **Enqueue drop path.** A's MergeDroppedGap (13-timing L1) and C's repeat-preserving drop gap (04 L4) are merged in order.
5. **Steering vs flush in steady state.** The steady depth at the backlog check is target+1. The excess age is under one period, below the max(100 ms, 3 periods) threshold.

## Verification (local, merged tip)

- **Build:** x64 Release, rc=0, for VideoProcessor-Test, VideoProcessor-VPRenderer, VideoProcessor-VPRendererBeta and VideoProcessor-ConfigTests.
- **CPU tests:** single-threaded, trx, GpuHardware/GpuOnScreen excluded.
  - Merged tip (bd355f15): 3229/3229 passed.
  - B+C merge: 3209/3209.
  - A2 on the same tree: 3228/3229. The one failure, DeckLinkConvertersSustained4K60PerformanceComparison, is timing-sensitive under load and passed alone.
- **Config tests:** off screen, 2 shards: 138 passed, 0 failed, 1 skipped (needs a real window). Gate passed.

## GPU tests for Claude

- `NlsHookFailureRetryLeavesNoStaleDejudderCache` (Category GpuHardware; passes on WARP).
- Filter: `/TestCaseFilter:"Name=NlsHookFailureRetryLeavesNoStaleDejudderCache"`

## Deferred

- **13-timing L3 kMaxSpanFrames** (gap-fill span as frames vs seconds): owner decision.
- **Classifier real-clip check** (dark 24p, Apple TV 3:2): no such footage in D:\hfr, and no CPU replay harness.
- **On-screen checks:**
  - the open-loop DWM vblank refinement and the lead estimate (laptop panel, render-ahead);
  - content intent with look-ahead 0;
  - motion fixes M1/M2/L3/L4 through the renderer harness.
- **NLS retry:** the adaptive-downscale grain field still gets the NLS map for the redraw frame.
- **Owner calls:**
  - a `chroma_downsampling` control;
  - the 500 Hz display bound;
  - DejudderPolicy's fixed film/video bands (30.5, 47.5-48.5 Hz), left for the classifier batch.
- **02-dv items outside this group's classifier/pairing scope** (M1 formatter repeat, M5 dv_trims, L3-L5, L7): not touched here.
- **Build environment:** E: does not exist on this machine. Builds use `MagewellSdkRoot=C:\Program Files\MWCaptureSDK 3.3.1.1596\SDKv3` (AGENTS.md names the E: path).
