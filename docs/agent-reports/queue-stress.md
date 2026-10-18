# Queue stress and fuzz harness (2026-10-18)

Branch `test/queue-stress` from `integ/round4` (92ffc884). CPU only, local only.

## Structure

`src/VideoProcessor-Test/QueueStressHarness.h` (model, generator, invariants, minimiser),
`QueueStressTests.cpp` (random run, hand-written scenarios, self-test, repros),
`QueueStressLaneTests.cpp` (LED module, intent observer, source-resolution monitor).

- **Scenario**: signal rate, content picture rate (pulldown when lower), display rate, switch
  set, timed events. Seeded splitmix64 (portable; no std distributions).
- **Source model**: capture slots with hardware timestamps and QPC callbacks. Events: starve
  (frames lost, 0.1-10 s), burst (N frames held, delivered at once), rate swing (resync),
  drift (+-0.5 % rate, QPC skew), callback jitter, timestamp reversal and jump, duplicate,
  drop, capture restart, format change, display rate change, render spike, present stall,
  Normal <-> Low latency. As OnVideoFrame: a frame older than 0.5 s gets the callback time.
  Ground truth (lost frames, hard discontinuities) travels with every frame.
- **Lane** (OnVideoFrame): `CaptureTimestampGap`, `Pulldown::Detector` + `FilmClock`, gap state.
- **Presentation**: render_ahead off: alpha queue paced by vblanks through the timeline source
  clock (`CanDequeue`, `TimelineQueueSteering`, `ShouldRecoverBacklog`, `MergeDroppedGap`,
  `LockedQueueTarget`, credit, resize purge, profile reset); low latency one frame per
  refresh. render_ahead on: FIFO back-pressure (`Schedule`, `PlanFrame`, `Gate`, cost windows,
  `SustainedLateness`, `GateEpoch`, `CreditLatch`), closed loop on simulated DXGI statistics
  or open loop (`LateRefreshes`). Each dequeue runs the pair decision (`GapFillPair`,
  `AdjacentPairTimes`), `MotionValidity::State` and the timeline.
- **LED lane**: the real `LedDriveModule` (compensate + CNTR assist / script / picture only,
  look-ahead on/off) on the manual clock behind `FakeProjector`, frames delivered with the
  queue faults (starve, burst, epoch restart, stall, profile).
- **Intent lane**: `ContentIntent::Observer` on the lane's kept stream (smooth pan, no cuts).
- **SR lane**: `SourceResolutionMonitor` on a bilinear 2x picture with stops, bunching, time
  reversal, resyncs and format changes.
- **Minimiser**: drops events one at a time while the signature still fails, shortens the
  run, resets switches; writes `Code()` (a ready regression test) to
  `%TEMP%\vp-qstress\repros.txt`, results to `results.txt` (`VP_QSTRESS_DIR` overrides).
- **Self-test**: five deliberate defects (break not a gap, fill across a no-fill gap, fill
  ignoring the span, no backlog flush, stuck dequeue after starvation) each trip their invariant.

Invariants: queue depth <= capacity; dequeue order; no freeze while above target; latency
within the derived steady bound outside fault + recovery windows; lane gap for every
discontinuity, no-fill for hard ones; no ordinary pair across missing frames, no pair across
a hard discontinuity, fill span within `MaxPairSpanFrames` (capture time); vectors invalidated
after every discontinuity; every queue drop marks the next frame; film clock never early,
never backwards inside a lock, 1 ms spacing and bounded lateness when quiet; cadence re-locked
20 s after the last fault; render-ahead extras within the gate's level, intervals <= 4,
pending <= 64, cost window bounded, gate back to full within T (cost window + known-cost
hold + up-after-down + a step per level); LED commands in range, CNTR 50 whenever the LED
is at its ceiling (beyond latency + settle + gaps), the bright-cut dump before the LPW1 jump
(script look-ahead: CNTR first), picture-only silent, exit restores; intent class changes only
at a discontinuity / first class / static UI; SR factors in (0, 1], native after a discontinuity.

## Runs

| run | count | simulated | wall |
|---|---|---|---|
| default random | 200 x 2-10 min | 1210 min | 4 s |
| hand-written (10 input types x 2 paths) | 20 | ~45 min | <1 s |
| LED / intent / SR | 16 / 12 / 4 | 12 / 36 / 12 min | 2 s / 26 s / 95 s |
| soak (`VP_QSTRESS_SOAK=3000`, `VP_QSTRESS_LED=300`) | 3000 + 300 LED | 17963 + 225 min | 58 s + 28 s |

Default suite: about 2.5 minutes. Related classes (AlphaQueuePolicy, QueueTimingFixes,
RenderAhead*, EpochBoundedQueue, CaptureResyncHardening, LiveEpochConvergenceController,
MotionValidity, PulldownReliability): 159 tests, all pass except the two Q1 repros.

## Findings

**Q1 (medium, open): a burst of stale frames steps the film clock back.** 142 of 3000 soak
scenarios, every one within 8 s after a burst. A capture that stalls and then delivers frames
older than 0.5 s: OnVideoFrame uses the callback time as their arrival
(`LibplaceboVideoRenderer.cpp:24250`), late by up to the stall; `FilmClock::Time`
(`PulldownDetector.h:747-787`) anchors on the (second) largest offset over its 2 s window, so
the stale frames share one time, every later frame is timed that late for 2 s, then the clock
steps back ~0.46 s inside the lock with no discontinuity (also seen as FILMCLOCK_SPACING /
FILMCLOCK_UNBOUNDED). Effects: timeline restarts, refused pairs, a render-ahead anchor taken
on late times. Minimal repro: 24p in 60 Hz, `burst@162.801 35` (seed 72):
`QueueStressRepros::StaleBurstFilmClockRunsBackwards`, and on the clock alone
`StaleBurstFilmClockAlone` (both fail). Fix options (not trivial): use the hardware age when
it is plausible, or mark a stale frame as a no-fill gap and re-anchor the clock at the next
fresh frame.

**Q2 (low, fixed under queue_timing_fixes): the profile resize purge marked no gap.**
`SetFrameQueueMaxSize` (switch to Low latency) popped queued frames without a gap on the next
one, unlike the backlog flush and overflow drop; only MotionValidity's sequence-hole check
caught it. Now a no-fill gap, cause "queue resize" (`ProfileSwitchPurgeMarksTheGap`).

**No failures** in queue depth, order, freeze, pairs, vector invalidation, render-ahead budget,
intervals, gate recovery, bounded structures, cadence re-lock, LED ranges / CNTR need / exit,
intent or SR.

**LED observation (not a defect of this harness's rules):** with the default
`cut_cntr_level` (dmd_clip_cntr 59) a bright cut keeps CNTR 59 and descends paired after the
LPW1 jump (by design, owner 2026-10-11); the first order oracle flagged it and was narrowed to
real dumps. With look-ahead on and `cut_lead` immediate (default), in-scene early-ramp sends
held the port so a cut's LPW1 jump left ~140 ms after the cut frame (seed 5).

## GPU scenarios worth running

1. 24p in 59.94 locked, 0.6-1 s capture-callback stall then burst (Apple TV + Magewell):
   look for timeline restarts / refused pairs ~2 s later (Q1).
2. render_ahead closed loop on the 3090: display 59.94 -> 60 -> 50 mid-film plus a 3 s
   heavy-shader spike: gate back to full within ~80 s, no flush cascade.
3. Normal <-> Low latency on 60p mid-stream: "queue resize" gap in vp.log, no motion
   artefact at the switch (Q2).
4. Laptop panel (open loop), 0.5 s present stall (fullscreen alt-tab): latency steady
   within ~10 s, no re-anchor loop.
5. HDMI source paused 3-10 s with compensate + CNTR assist: CNTR 50 within
   led_idle_restore_s, LED resumes on the first frames.
