# Q1: a stale burst stepped the film clock back (2026-10-18)

Branch `fix/q1-stale-burst` from `test/queue-stress` (93f20fe1). CPU only, local only.

## Cause

A capture that stalls and then delivers its held frames at once hands over frames older
than 0.5 s. OnVideoFrame gave those frames the callback time as their arrival, so they
were late by up to the stall. `Pulldown::FilmClock` anchors on the largest offsets in its
2 s window. Every later frame was then timed about 0.46 s late for 2 s. When the stale
offsets left the window, the clock stepped back inside the lock with no discontinuity.
The result was a timeline restart, refused motion pairs and a render-ahead anchor taken on
late times. Both owner paths are affected: 24p in 29.97 (streaming apps, one repeat in
five) and 24p in 59.94 (3:2).

## Fix

1. **The arrival** (`queue_timing_fixes`, default on). `CaptureArrival::Tracker`
   (`CaptureTimestampGap.h:94`), used by `OnVideoFrame` (`LibplaceboVideoRenderer.cpp:24308`).
   A stale frame keeps its hardware time when that time is plausible. Plausible means all of
   these hold:
   - the age is known;
   - there is no capture timestamp break;
   - the hardware time does not run backwards in a chain that started at a fresh frame. So a
     timing clock on another base, which makes every frame look old, is never trusted.

   Otherwise the frame keeps the callback time, as before. The film clock then stays
   continuous through the burst.
2. **The delivery gap.** Two frames get a no-fill gap ("stale capture delivery"):
   - the first stale frame of a run;
   - the first fresh frame after a run timed on the callback.

   The gap is applied after the pulldown detector (`:24537`), and is kept across dropped
   repeats. So the cadence lock is kept, and no hiccup-repair (gap-fill) pair spans the
   stall.
3. **The film clock's late-arrival guard** (`pulldown_fixes`, `PulldownDetector.h:728-860`,
   `SetLateGuard` at `LibplaceboVideoRenderer.cpp:24429`). This is the safety net when the
   hardware time can't be used.
   - An offset more than 0.25 s above the window's anchor is a late arrival. The frame is
     never timed before it arrived, but its offset stays out of the window.
   - If late arrivals last 1 s of film, they become the new level and the clock steps
     forward.
   - The clock never runs backwards while it is in use. The on-time frames after a late run
     catch up within that run's lateness.

No rates are hard-coded. The thresholds are in seconds: OnVideoFrame's existing fresh limit
and the clock's own window.

## Harness

- The lane now decides the arrival with the tracker.
- `RunOptions::legacyQ1` and `VP_QSTRESS_LEGACY_Q1=1` run the renderer as it was before
  this fix.
- 24p in 29.97 and 24p in 30 were added to the random rates.
- The burst arrival window now uses the signal rate at the burst, not the initial rate.
- The FILMCLOCK_* signatures are no longer "known".
- New tests:
  - `StaleArrivalTrackerKeepsPlausibleHardwareTime`
  - `FilmClockLateGuardBoundedCatchUpAndLastingStep`
  - `StaleBurstFilmSoak`: 24p in 29.97 and 24p in 59.94, display 60 Hz, 1-4 bursts of 2-60
    frames. It runs each scenario with the fix, with the guard only, and as before the fix.

## Results

| run | before | after |
|---|---|---|
| random soak 1000 (old harness, 93f20fe1 build) | 38 FILMCLOCK_BACKWARDS (+5 spacing, +5 unbounded) | - |
| random soak 1000 (new harness, same scenarios, legacyQ1 vs fixed) | 66 FILMCLOCK_BACKWARDS | 0 film clock failures |
| random soak 3000 | - | 0 film clock failures |
| burst soak, 24p in 29.97 at 60 Hz, 500 scenarios / 1246 bursts | 468 with reversals | 0 failing, lock never lost |
| burst soak, 24p in 59.94 at 60 Hz, 500 scenarios / 1254 bursts | 384 with reversals | 0 failing, lock never lost |
| guard only (queue_timing_fixes off), both paths | - | 0 film clock failures |

The only remaining soak signature is QUEUE_DROP_UNMARKED, and only with
`queue_timing_fixes` off. That is by design: Q2 is fixed by the switch.

Related classes: 263 tests pass. That covers all three QueueStress classes, including the
default 200 random scenarios and both Q1 repros. It also covers CaptureTimestampGap,
FilmClockRebase, FilmClockToggle, Hunt2CadenceClass*, PulldownReliability, PulldownCorpus,
PulldownLockChangeAdjacency, GapSpan/TimelineSpan/Dejudder matrices, InterpolationTimeline,
MotionValidity, Hunt2MotionHistory, RenderAhead*, AlphaQueuePolicy,
AlphaCadenceCorrectionPolicy, CadenceIntervalEstimate, QueueTimingFixes, QueueLookahead,
EpochBoundedQueue, CaptureResyncHardening, LiveEpochConvergenceController and the capture
bug-hunt classes.

## GPU check worth running

Apple TV through Magewell, 24p in 29.97 and in 59.94, locked. Cause a 0.6-1 s
capture-callback stall. In vp.log, look for "Capture: stale capture delivery (... hardware
time ...)". There should be no timeline restart or refused pairs about 2 s later, and the
pulldown lock should be kept.
