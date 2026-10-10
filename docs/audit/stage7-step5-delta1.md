# Audit — Stage 7 step 5: delta-1 amendments

Diff audited: 8db148cf..f88e15d4 (b122fca0 docs, f88e15d4 code)
Build: OK — full x64 Release /t:Rebuild of VideoProcessor.sln at HEAD 3542215b, 0 errors, no warnings in audited code (MagewellSdkRoot set to the installed MWCapture SDK 3.3.1.1596; it is not set by default on this PC).
Tests: native suite (vstest, VideoProcessor-Test.dll) 2042 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted — known environmental); VideoProcessorConfigTests 87 passed, 2 failed (the two known Config-app RPC tests). One run at HEAD, covering all steps.

Spec used: VP-led-drive-delta-1.md items 1–6 as amended by
delta-1-answers 1–8, answers-2 B1–B6, guide §9 "Step 5 (delta-1)
implementation notes".

## Blockers
- None new (the restart-on-edit blocker is in stage7-step4-compensate.md).

## Should fix
- led/LedDriveModule.cpp `StepCompensateLocked` (~line 215) — output frames
  and source frames are mixed. `latency` is in *output* frames (80 ms = 5 at
  60 Hz) but is added to the *source* sequence number to find "the frame the
  command is for" (`predictions.upper_bound(sequence + latency)`). For 60p
  sources they are the same; for 24p film in the 60 Hz cinema queue, 5 source
  frames is 208 ms, not 80 ms. With today's ~3-frame queue the code then
  simply uses the furthest queued frame (~125 ms ahead), so the LED can start
  dimming toward the next scene up to ~45 ms too early, outside the early-ramp
  highlight bound. Small today; grows with Stage 5's deeper queue. Delta-1
  item 2 / answers 1 (command `led_latency_ms` before its frame). Convert
  latency to source frames (latency_ms × source fps) for the lookup.
- led/LedLookahead.cpp `ObserveFrame` (~line 109) — same mix-up in the raw
  path's clock: `sourceSequence × FramePeriodMs()` multiplies a source frame
  number by the *output* frame period. At 24p every time window on the
  predicted statistics is 2.5× too long in real time (`highlight_persist_ms`
  250 → ~625 ms, `apl_persist_ms` 100 → 250 ms), so predicted flash rejection
  and "act at once on a darkening" are slower than configured. Use the source
  frame period (or the frame timestamp).
- The tests that cover this (WithLookaheadTheCommandPrecedesItsFrameByLedLatency,
  WithoutLookaheadTheGainLandsLedLatencyAfterTheCommand) render exactly one
  frame per source frame, so they cannot see it. Add a 24p-in-60 Hz case.

## Notes
- vprenderer/LibplaceboVideoRenderer.cpp ~13935 — besides BeginFrame
  (gain in) the per-frame block also calls `SetRawTransfer` to hand the
  colour mapping to the lookahead. It sits inside the gain-in call site, so
  the "three call sites" rule is kept in spirit; mention it in the PR notes.
- Lookahead observer (`QueuedFrameStatistics::IObserver`) is only subscribed
  in `mode: compensate`, so `script` mode cannot use predictions.
- delta-1 answers 4: `active_picture_lookahead_frames: 8` also turns on the
  fork's own lookahead inspection for `[queue.Normal]` (CPU cost even with
  the LED module off) — recorded in guide §9.

## Verified OK (delta-1 item 6 and answers)
- Flux table: piecewise linear, clamped to end values outside its range,
  linear default with one startup log line naming `lpw_max` (`?M`, else
  `led_ceiling`); used for `gain_led` and for the headroom limit
  (FluxTableInterpolatesClampsAndDefaultsToLinear).
- `led_latency_ms` (80) in `[projector.Default]`; `led_response_ms` and
  `lead_ms` rejected with a message naming the new place
  (LatencyMovesToTheProjectorAndOldKeysAreRejected). Pure dead time in whole
  output frames, no lag (answers 1); gain switches with the modelled level.
- Compensate runs per output frame at `rate_up`/`rate_down` levels per second
  with the `deadband` (hysteresis: stops a resting LED starting, lets a ramp
  finish) — CompensateRampsAtTheRateLimitsAndHoldsInsideTheDeadband; cuts
  ramp, fades follow the APL ratio capped by `rate_down`, no deadband, gain
  held (CutsAreRampedAndFadesAreCappedByRateDown).
- Early ramp: dimming ahead of a cut stops at the current scene's highlight
  limit within `highlight_tolerance_stops`; brightening unconstrained
  (EarlyRampDimsOnlyToTheCurrentHighlightLimitAndBrightensFreely). Trigger: a
  cut in the visible queue or an APL step ≥ `cut_threshold` (guide §9).
- Raw lookahead: luma through a 1D transfer built with `pl_tone_map_sample`
  from the renderer's own colour-mapping settings, including `output_gamma`
  and the module's `gamma_boost`; rebuilt when the mapping changes; cut/fade
  on raw luma; correction ratio over 3 s, reset on rebuild
  (RawFramesAreMeasuredThroughTheTransferLikeTheGpuPass,
  LedRawTransferMatchesTheRenderedOutput, CorrectionRatioConvergesOnASyntheticMismatch).
- `script` mode keeps the exact PRE_OUTPUT measurement and ignores
  predictions (ScriptModeIgnoresTheLookahead); tick-for-tick test unchanged.
- Effective lookahead clamped to frames actually queued
  (EffectiveLookaheadIsClampedToTheFramesActuallyQueued).
- Third call site is a neutral observer: QueuedFrameStatistics.h and the
  walk's added lines contain no LED reference in names or comments.
- Test config: `led_latency_ms: 80`, `active_picture_lookahead_frames: 8` in
  `[queue.Normal]` only, ramp keys added; `Low_Latency` frame counts unchanged.
