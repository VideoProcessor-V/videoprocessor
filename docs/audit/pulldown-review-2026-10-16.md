# Pulldown (cadence) detection review - 2026-10-16

Read-only review of `PulldownDetector.h` and its use in `LibplaceboVideoRenderer.cpp`, done for
the owner's question "on by default only if reliable on real 3:2; disengages clean; fade / resync
/ jitter rules apply". **Verdict: not ready to be on by default.** It is fail-safe against false
locks, but the disengage is not clean and every small break costs a 2 s unlock.

## False locks (fail-safe)

| Content | Result |
|---|---|
| Static / black / paused | never (no new frame in the cycle) - tested |
| Animation on twos / threes | never (implied rate < 15 fps) |
| 30p in 60 | locks as 30p film by design; de-judder then follows `dejudder_even_rates` |
| 30 fps menus / UI, 20 fps spinners | lock; harmless (only real repeats skipped), leaving goes through D3 |
| Video fades | no exact repeats at 60p; a uniform 8-bit card fade could lock (low risk) |
| Random repeats / timestamp jitter | never |

## Defects

- **D1 - the motion reset is lost when the lock is gained on a repeat frame.** `gapBefore` is a
  local set on a lock change; when that frame is an expected repeat it is dropped before queueing,
  so the next kept frame carries no gap and no motion invalidation (about 3 times in 5 at 3:2).
  The same early return skips `BreakContinuity` for dropped frames. Fix: a pending-gap member
  applied to the next kept frame.
- **D2 - the queue target is resolved at the signal rate while a locked queue holds film frames.**
  A 200 ms target becomes 12 frames, about 500 ms of 24p (x cycle/new: 2.5 at 3:2). Latency jumps
  at every lock / unlock; after an unlock 60 Hz frames may pile up behind film frames and trigger a
  backlog flush (a drop + gap) - probable, not measured.
- **D3 - the film clock is wrong while unlocking and never re-phases.** A phase shift of one frame
  (edited telecine, pause / resume, seek without resync) mistimes frames for about 3 cycles
  (250 ms at 3:2; 500 ms at 24-in-30) before the lock drops, then 2 s unlocked, then a relock
  (a second invalidation). No attempt to re-phase onto a rotation of the pattern.
- **D4 - one missed capture frame resets the lock for 2 s**, although the renderer knows how many
  frames were missed (it could advance the phase and keep the lock, verified over the next cycle).
- **D5 - small rate mismatches** (23.976 in 60.000, 24 in 59.94, 25 in 59.94 - frame-rate matching
  off) slip one film frame about every 40 s; each slip costs D3 + 2 s.

## Risks

- Film clock jitter: the anchor is the largest offset over 2 s; a late arrival larger than one
  film period with the `callbackQpc` fallback could step time backwards (hardware timestamps fine).
- Frame-count windows downstream (scene cooldown, the 240-frame subtitle capture, queue minimums)
  last 2.5x longer while locked - against "general, not fixed".
- An update frame shares its film frame's timestamp, so the previous film frame gets no motion pair.
- The signature samples 64 luma rows: clean CGI moving between sampled rows, chroma-only changes or
  device subtitles under ~34 px could hash equal; in an R slot such a change is dropped.

## Test needed before default-on

No telecined film exists in `D:\hfr` (native 120p / 50p clips): synthesize 3:2 / 2:2 / 24-in-30 /
24-in-50 from the 120p clips (decimate by 5) and the 50p y4m (by 2), with scenario events (fade to
black and back, cut, edit = one dropped / duplicated signal frame, single missed frame, pause and a
restart at a random phase, 23.976-in-60 slip, subtitle change on an R slot, 60p video, 30p segment,
animation on twos, ±3 ms jitter and a 15 ms spike). CPU only (reads the 64 sampled rows). Reuse
`FeedPulldown` / `CarriedPattern` (ConfigFileTests.cpp) and the footage readers in
SourceResolutionDetectorTests.cpp.

Pass criteria: no new picture ever dropped; zero locks in video / stills / twos / random; lock within
FramesToLock + one cycle; no unlock through fades, black, cuts or subtitles; locked >= 98% of film
time; wrong-time frames per phase event <= 2 (today up to ~15); monotonic film clock spaced 1/f within
±1 ms under ±3 ms jitter; every lock change marks the next kept frame (D1). Then the owner's Apple TV
recordings (Mini Recorder; `frames.csv` has `exact_repeat`) replayed through the detector: >= 98%
locked, zero false locks, every unlock with its cause. Plus a hardware check of queue depth and
backlog flushes across lock / unlock (D2).

Order: fix D1 (small), D4 (keep the lock across counted misses), D3 (re-phase), then D2 (resolve the
queue target at the film rate while locked) - each behind a switch in Fixes under test - then the
synthesized test, then the recordings.
