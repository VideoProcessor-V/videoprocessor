# Real-footage suite: timing, cadence, classifier and source resolution (2026-10-18)

Branch `test/real-footage-suite`, worktree `D:\codex\videoprocessor\realclip`, base `integ/2026-10-17` at ca7e3670. That base already includes the merge of `fix/hunt2-timing` 07a1afe2 (`queue_timing_fixes`, `cadence_class_fixes`, `motion_history_fixes`). Everything ran locally against the clip library `D:\testclips` (README 2026-10-18) with ffmpeg 9.0.2 from `D:\tools\ffmpeg\bin`.

## Suite structure

### Manifest

`tools/testclips/make_clip_manifest.py` parses the library README's tables into `D:\testclips\manifest.json`. It lists 53 clips. For each clip it records:
- path, rational rate and frame count;
- interlace and VFR flags;
- cadence kind, holds and picture rate;
- typed events: drop, duplicate, black, frozen, pts_gap, rate_change, jitter, segment, phase_break, cadence_slip, fade_in/out and pan;
- for the 3-minute trims, scene-cut ground truth. This comes from the library's own `_scripts\<src>_cuts.txt`, mapped onto each clip's clock.

With `--probe`, the generator also records the size and packet count from ffprobe. All 53 match.

### Reader

`src/VideoProcessor-Test/RealClipLibrary.h` contains a minimal JSON reader and the decode path:
- ffmpeg decodes each clip to 8-bit luma over a pipe, at the clip's own size. An optional ffmpeg filter that keeps the size can be applied.
- Frame timestamps come from the packet pts that ffprobe reports.

Environment variables:

| Variable | Purpose |
|---|---|
| `VP_TEST_CLIP_MANIFEST` | Manifest path |
| `VP_TEST_FFMPEG` | ffmpeg path |
| `VP_TEST_FFMPEG_THREADS` | ffmpeg decoder threads |
| `VP_TEST_CLIP_FILTER` | Run a subset: clip ids containing this substring |
| `VP_TEST_REQUIRE_FOOTAGE` | Fail instead of skip when the library is missing |
| `VP_TEST_RESULTS_DIR` | Where the per-clip table is written |

If the library is missing, every test logs SKIP-NO-FOOTAGE.

### Harness

`src/VideoProcessor-Test/RealFootageSuiteTests.cpp` decodes each clip once per process. It feeds the frames to four configuration lanes at the same time:
- fixes on;
- `pulldown_fixes` off;
- `cadence_class_fixes` off;
- `timing_continuity_fixes` off.

Each lane repeats what `LibplaceboVideoRenderer::OnVideoFrame` and the dequeue do:
- `CaptureTimestampGap` against the nominal period, and the cumulative measured rate.
- `Pulldown::Detector` and `FilmClock`, wired to the switches as the renderer wires them.
- The enqueue gap state.
- `MotionValidity`: Invalidate on gapBefore, then NoteKept.
- The hiccup-repair pair: `GapFillPair` and `AdjacentPairTimes`. These ran with `interpolation_fill_gaps` on; its default is off.
- `InterpolationTimeline::Timeline`: restarts and re-anchors.
- `DejudderPolicy::MatcherHistoryChained`.
- `ContentIntent::Observer` with the default `source_class` (legacy) and a 5 s hold.
- `SourceResolutionMonitor` with latch fixes on and off:
  - one tile copy every 0.5 s on the frame clock, analysed 0.1 s later;
  - `AutoFactors` and `AutoTransition` on every frame.

After the lanes, the kept frames go through a presentation simulation of the alpha queue built from the `AlphaQueuePolicy` gate, steering and flush:
- display rates 59.94, 60, 50, 23.976 and 120 Hz;
- a display clock offset of 0 and ±100 ppm;
- ±2 ms callback jitter;
- `queue_timing_fixes` on and off.

### Signal models

**Direct with transport repeats.** Encoded repeats are not bit-exact, because of codec noise. To model what a capture would see, a frame becomes a bit-exact repeat when both of these hold:
- its mean difference from the previous frame is at most 0.75 codes;
- its worst 16x16 block differs by at most 8 codes.

The thresholds were measured on `tos_32_5994p`:
- true repeats reach at most 0.53 mean and 5.7 per block;
- new pictures are at least 0.89 mean at the 1st percentile.

A burned-in counter still counts as a new picture.

**Player.** A player outputs the clip at a fixed 59.94, 60 or 50 Hz and makes bit-exact repeats. It schedules with a quarter-period of slack. Arrivals carry ±1 ms of QPC jitter, while the hardware clock stays clean.

**Rate change.** A captured rate change is an HDMI mode change, so it is modelled as a resync.

### Tests

- `RealFootageSuiteTests`: 10 methods.
  - manifest
  - cadence and FilmClock
  - splices
  - classification
  - discontinuities
  - pans
  - drift queue
  - source resolution
  - player pulldown
  - queue across display rates
- `RealFootageRegressionTests`:
  - two minimal tests for the bugs fixed here; they need no footage;
  - one ignored repro for R3-1.

A full run takes about 47 minutes, single-threaded. It is decode-bound, with ffmpeg on 3 threads.

### Existing D:\hfr-only tests

- **`PulldownReliabilityTests.RealFootageTelecined`** now also takes any 8-bit 4:2:0 y4m in `VP_TEST_REAL_FOOTAGE`, at the size and rate in its header. The fixed `D:\hfr` list is unchanged. With `VP_TEST_REAL_FOOTAGE=D:\testclips\film` it now passes: 5 clips, 13 cadences. Before, it failed with "no clip found".
- **`PulldownCorpusTests`** already reads any y4m, so it needed no change.
- **SourceResolutionLatch, SourceResolutionDetector and HdrInference studio tests** are unchanged. They build 4K-specific upscale classes from named clips. Their 1920x800 counterpart is the source-resolution part of this suite.

## Results (fixes on)

140 rows PASS and 1 row is KNOWN BUG (round 3). The other 131 rows are INFO rows that compare the configurations.

**Cadence: 39/39 pass.**
- Every 2:3 and 2:2 pulldown and every rate conversion locks at FramesToLock - 1: frames 59, 74, 95, 99, 119 and 239.
- Locked share is 100% and the film rate is exact.
- FilmClock (C7, measured in quiet stretches): 0 ms spacing error, monotonic, never early.
- There is no false lock on:
  - native 23.976p, 24p and 25p;
  - Meridian 29.97p and 59.94p;
  - the dark film variant;
  - PsF;
  - the mixed clip, once fix 1 below is in.
- Field telecine: `cosmos_32_5994i` has one lock of 39 frames, at the true 23.976 rate. Animation held on twos produces one woven repeat in five.

**Splices: 3/3 pass.**
- Phase jumps: the lock re-phases in place 7-8 frames after each of the 5 breaks.
- 2:2 slips: the lock re-phases in place 3 frames after each of the 4 slips.
- Film to 30p: each segment locks at exactly the earliest frame its own repeat flags allow.

**Classifier: 32/32 pass.**
- All film clips, the dark film, the telecines and PsF classify as film.
- Meridian 59.94p classifies as video.
- The 59.94p pans stay video with sports pending, because the class changes only at a cut.

**Discontinuities: 8/8 pass.**
- Missed frames:
  - flagged at exactly frames 240, 599, 775, 997 and 1196;
  - each gap is one frame;
  - at each, the vectors are invalidated and a gap pair is offered.
- Duplicates are seen at frames 241, 602, 780, 1004, 1005 and 1206.
- The black and frozen 2.5 s gaps raise no event, and their repeats never lock.
- The 2.5 s pts gap at 720:
  - one break;
  - the vectors are invalidated;
  - no pair spans it;
  - the timeline restarts.
- ±2 ms jitter produces no event of any kind.
- The rate change produces one resync at 720. Played at 59.94 Hz, the lock is lost 5 frames after the change and never comes back.

**Motion: 10/10 pass.** Pans cause 0 invalidations and 0 refused pairs. Fades over a pan produce no source-resolution cut.

**Queue and drift: 21/21 pass.** The 10-minute drift clip, captured and played as 3:2, produces 0 flushes at every display rate and ppm offset, and the queue depth stays at or below target + 2.

**Player pulldown: 8/8 pass.**

**Source resolution: 18/18 pass, plus 1 known bug.**
- Native footage never engages.
- Cut recall against the scene ground truth is 0.67-0.95. The latch fixes find at least as many cuts as the old rule, and usually more (Cosmos: 14 vs 6).
- Flashes add no cut.
- The upscaled long take is now declared at 10.0 s.
- The upscaled ToS trim engages 0.42 s after a real cut.

## Real bugs

### Fixed here, behind the existing switches

**1. Mixed-cadence pulldown lock (`cadence_class_fixes`).**
- The clip carries 24p film as 2:3 plus a 30p graphic as 2:2. The detector locked it as "41.958p film (7 of 10)": the holds 2,2,2,1,1,1,1 pass `IsEvenPattern`.
- Fix: a lock now also needs a maximally even pattern. This is `Pulldown::IsMaximallyEvenPattern` with `Settings::maximallyEvenPattern` in `PulldownDetector.h`. In LibplaceboVideoRenderer it is wired as `= classFixes`.
- Every real pulldown is unchanged. This was checked with the corpus, reliability, Hunt2 cadence and full CPU test suites.
- Test: `MixedCadenceOverlayNeverLocksAsFilm`.

**2. Source-resolution cut judge at stream start (latch fixes path).**
- `CutJudge` started with motion 0. A moving opening shot therefore read as two cuts by 1.5 s, giving three shots. Under the "minimumShots OR minimumSeconds" rule, the upscaled long take was declared at 1.5 s instead of after 10 s.
- Fix: the first shape at the start of a stream, or after a discontinuity, now starts at `cutMotionAfterCut`, the same as after a cut. The change is in `Tracker::Update`, `SourceResolutionDetector.cpp`.
- Test: `StreamStartMotionIsNotACut`.

### Deferred to bug hunt round 3

**R3-1: in-shot camera moves and scrolls read as cuts, so auto engages mid-shot.**
- Where: `SourceResolutionDetector.h` `Tracker::Settings` (`cutDecorrelation` 0.4, `cutJump` 0.2, `cutMotionMargin` 0.3) and `CutJudge::Judge`.
- Repro: `cosmos_longtake_23976p_44s` with the filter `scale=iw/2:ih/2:flags=area,scale=iw*2:ih*2:flags=lanczos`.
  - The take is declared at 10.0 s.
  - The dolly to an extreme close-up at 28.5 s counts as a cut. The cuts are at 28.53, 31.03, 34.53 and 35.03 s, in a take with no scene cut.
  - Auto engages at 28.65 s "at a cut" with no fade. The crossfade was due at 30.1 s.
- The credits scroll also counts 6 such cuts.
- Real camera moves exceed the latch-fixes calibration, which allows motion decorrelation up to 0.78 and a level jump up to 0.11.
- Ignored test: `LongTakeCameraMoveIsNotACut`.

**R3-2: native trims get short-lived lower declared readings.**
- They never engage, because confidence stays below 0.8.
- ToS: 0.75 at 69 s and at 100 s. Cosmos: 0.35 at 34 s and 0.5 at 131 s.
- Cause: fast action produces many cuts, so three shots are reached quickly.
- This is a near miss of the owner rule "never re-base native".
- Repro: the "declared ..." values in the source-resolution rows of the table below.

**R3-3: no PsF or field-telecine detection.**
- `tos_22_50i_psf` (25p film carried in 50i) and the 59.94i telecines are deinterlaced as video.
- `TemporalRenderer.h:57` deinterlaces every interlaced input unless `deinterlace_algorithm=weave`. Nothing weaves PsF or inverse-telecines fields.
- This is a feature gap on the GPU/renderer side.

### Notes (not VP bugs)

**Library recording without labels.** `PulldownCorpusTests.AppleTvRecordingsReplay` gives 91.18% locked on `rec_film_30p_alternating_5994`, with every unlock explained. That recording has no `labels.csv`, so its three 2 s relocks after cadence changes count against the 98% criterion. The library needs to add the labels; VP is not at fault. The other 5 recordings are 100% locked.

**Pulldown baked into encoded video is never detected.** A player that sends decoded 59.94p, with the repeats baked into the encoded stream, is never detected: exact-repeat locks are 0 on every clip. This is by design: VP's detector needs bit-exact HDMI repeats.

## Test expectations corrected (first-pass failures that were the test's error)

- **Rate-ratio tolerance.** At 0.2% it took 24p in 25 for 23/24. It is now 0.11%, which covers only the 1000/1001 family.
- **Telecine and PsF.** A lock is allowed at the true film rate. Held animation frames produce real whole-frame repeats.
- **FilmClock C7.** Spacing is checked only in quiet stretches, as in the corpus's own C7: at least 2.5 s after any lock change, re-phase, gap, film update or rate change. 24.000p content in a 1001 signal slips one signal period by nature.
- **Film/30p splice.** Lock time is measured against the earliest lock the delivered repeat flags permit. Meridian's static 30p stretches delay it.
- **Pans.** Sports-speed pans without cuts stay video with sports as the candidate. The first class is taken before any cut, as `ContentIntentTests` expects.
- **Mixed clip.** It is expected to classify as video, because the whole picture changes about 42 times a second.
- **Scene-cut ground truth.** The scene-score list misses hard cuts (ToS at 51.5 s), so cuts outside the list are reported, not failed.
- **Harness models.**
  - Hardware timestamps no longer carry the QPC jitter.
  - The player model now has a quarter-period of slack. Without it, 1 ms container rounding produced a 60-cycle "41/60" lock.

## Fixes on vs fixes off

**`pulldown_fixes` off:**
- Each phase break unlocks and needs about 2 s to relock.
- Locked share falls to 0.85 on the phase-jump clip and 0.73 on the 2:2 slip clip.
- rate_59.94, rate_29.97 and rate_119.88 lock 5-10 times in 40 s, against once with the fixes.

**`cadence_class_fixes` off:**
- The mixed clip locks as 7/10 at 41.958 Hz and is classified as film.
- On locked clips the first class comes later: 7.0 s instead of 5.2 s.

**`timing_continuity_fixes` off:** the 2.5 s pts gap no longer invalidates the vectors, so motion pairs survive across the break.

**`queue_timing_fixes` off:**
- The drift clip flushes on a 23.976 Hz display: 4 times, median 151 s apart, when captured; 8 times, median 115 s apart, when played as 3:2.
- The 3-minute trims flush up to 10 times (the 2:2 50p trim).
- With the fixes on, there are 0 flushes everywhere.
- This vblank-quantised simulation did not reproduce the roughly 80 s cycle at 24-50 Hz displays. The continuous model in `QueueTimingFixesTests` does.

**Latch fixes off:**
- Fewer real cuts are found: 6 against 14 on Cosmos.
- The long take picks up 6 flash cuts.
- The upscaled long take never engages, because 3 shots are needed.
- No false declared readings occur.

## Other verification

- **Full CPU suite**: GpuHardware and GpuOnScreen excluded, `VP_PULLDOWN_RECORDINGS=D:\testclips\pulldown`.
  - 3343 of 3347 passed, 3 failed, 1 skipped.
  - Two of the failures, `DeckLinkConvertersSustained4K60PerformanceComparison` and `...RGBToP010...4KSmokeTest`, are timing-sensitive under load. Both pass when run alone.
  - The third, `AppleTvRecordingsReplay`, fails on the unlabelled library recording described above.
- **Build**: x64 Release solution, rc=0.

## GPU tests that would add value (not run, the GPU is reserved)

- **`RendererCaptureHarnessTests.RecordedStreamReplaysThroughTheRenderer`** with `VP_TEST_STREAM_DIR` set to `D:\testclips\pulldown\rec_24p_in_5994_phase_jumps` and then to `...\rec_24p_in_5994_missed_frames`. This exercises the real renderer's pulldown, D3 and D4 path.
- **A renderer replay of `cosmos_longtake@upscale2x`.** This shows R3-1, the mid-shot engagement, on screen, along with the crossfade mix.
- **TemporalRenderer on `tos_22_50i_psf_3min`, bwdif vs weave.** This would quantify the R3-3 damage.
- **`NlsHookFailureRetryLeavesNoStaleDejudderCache`**, carried over from the hunt-2 report.

## Full per-clip table

From the final run. The drift and queue rows come from the rerun with ±2 ms callback jitter.

| Category | Clip run | Check | Result | Numbers |
|---|---|---|---|---|
| manifest | all | files, sizes, typed events | PASS | 53 clips, 0 missing, 0 unsized; drops 5 dups 5 phase breaks 5 slips 4 segments 4 |
| cadence | cosmos_23976p_3min | no false lock (C2) | PASS | locks 0 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 5; none |
| cadence | cosmos_23976p_3min | pulldown_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | cosmos_23976p_3min | cadence_class_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | cosmos_23976p_3min | timing_continuity_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | cosmos_32_5994i_3min | no lock but at the film rate (field telecine) | PASS | locks 1 unlocks 1 rephases 0; exact-repeat locks 0; transport repeats 78; lock@4022(4/5 23.976) unlock@4061 |
| cadence | cosmos_32_5994i_3min | pulldown_fixes off | INFO | locks 1 unlocks 1 rephases 0; lock@4022(4/5 23.976) unlock@4052 |
| cadence | cosmos_32_5994i_3min | cadence_class_fixes off | INFO | locks 1 unlocks 1 rephases 0; lock@4022(4/5 23.976) unlock@4061 |
| cadence | cosmos_32_5994i_3min | timing_continuity_fixes off | INFO | locks 1 unlocks 1 rephases 0; lock@4022(4/5 23.976) unlock@4061 |
| cadence | cosmos_32_5994p_3min | pulldown lock (C3/C5) | PASS | expected 2/5; first lock at 119 (<= 130), locked share 1.0000, film 23.976 Hz; locks 1 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 6489; lock@119(2/5 23.976) |
| cadence | cosmos_32_5994p_3min | film clock (C7) | PASS | 3132 samples, max spacing error 0.000 ms, 0 over 1 ms (0 outside re-phase windows), 0 non-monotonic, max early 0.000 ms |
| cadence | cosmos_32_5994p_3min | pulldown_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@119(2/5 23.976) |
| cadence | cosmos_32_5994p_3min | cadence_class_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@119(2/5 23.976) |
| cadence | cosmos_32_5994p_3min | timing_continuity_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@119(2/5 23.976) |
| cadence | cosmos_longtake_23976p_44s | no false lock (C2) | PASS | locks 0 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 0; none |
| cadence | cosmos_longtake_23976p_44s | pulldown_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | cosmos_longtake_23976p_44s | cadence_class_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | cosmos_longtake_23976p_44s | timing_continuity_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | meridian_2997p_3min | no false lock (C2) | PASS | locks 0 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 397; none |
| cadence | meridian_2997p_3min | pulldown_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | meridian_2997p_3min | cadence_class_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | meridian_2997p_3min | timing_continuity_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | meridian_5994p_3min | no false lock (C2) | PASS | locks 0 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 2160; none |
| cadence | meridian_5994p_3min | pulldown_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | meridian_5994p_3min | cadence_class_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | meridian_5994p_3min | timing_continuity_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_22_50i_psf_3min | no lock but at the film rate (PsF) | PASS | locks 0 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 30; none |
| cadence | tos_22_50i_psf_3min | pulldown_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_22_50i_psf_3min | cadence_class_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_22_50i_psf_3min | timing_continuity_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_22_50p_3min | pulldown lock (C3/C5) | PASS | expected 1/2; first lock at 99 (<= 104), locked share 1.0000, film 25.000 Hz; locks 1 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 4350; lock@99(1/2 25.000) |
| cadence | tos_22_50p_3min | film clock (C7) | PASS | 4207 samples, max spacing error 0.000 ms, 0 over 1 ms (0 outside re-phase windows), 0 non-monotonic, max early 0.000 ms |
| cadence | tos_22_50p_3min | pulldown_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@99(1/2 25.000) |
| cadence | tos_22_50p_3min | cadence_class_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@99(1/2 25.000) |
| cadence | tos_22_50p_3min | timing_continuity_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@99(1/2 25.000) |
| cadence | tos_23976p_3min | no false lock (C2) | PASS | locks 0 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 30; none |
| cadence | tos_23976p_3min | pulldown_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_23976p_3min | cadence_class_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_23976p_3min | timing_continuity_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_24p_3min | no false lock (C2) | PASS | locks 0 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 30; none |
| cadence | tos_24p_3min | pulldown_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_24p_3min | cadence_class_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_24p_3min | timing_continuity_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_25p_speedup_3min | no false lock (C2) | PASS | locks 0 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 30; none |
| cadence | tos_25p_speedup_3min | pulldown_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_25p_speedup_3min | cadence_class_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_25p_speedup_3min | timing_continuity_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_32_5994i_3min | no lock but at the film rate (field telecine) | PASS | locks 0 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 45; none |
| cadence | tos_32_5994i_3min | pulldown_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_32_5994i_3min | cadence_class_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_32_5994i_3min | timing_continuity_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_32_5994p_3min | pulldown lock (C3/C5) | PASS | expected 2/5; first lock at 119 (<= 130), locked share 1.0000, film 23.976 Hz; locks 1 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 6510; lock@119(2/5 23.976) |
| cadence | tos_32_5994p_3min | film clock (C7) | PASS | 3132 samples, max spacing error 0.000 ms, 0 over 1 ms (0 outside re-phase windows), 0 non-monotonic, max early 0.000 ms |
| cadence | tos_32_5994p_3min | pulldown_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@119(2/5 23.976) |
| cadence | tos_32_5994p_3min | cadence_class_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@119(2/5 23.976) |
| cadence | tos_32_5994p_3min | timing_continuity_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@119(2/5 23.976) |
| cadence | tos_credits_scroll_23976p_50s | no false lock (C2) | PASS | locks 0 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 0; none |
| cadence | tos_credits_scroll_23976p_50s | pulldown_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_credits_scroll_23976p_50s | cadence_class_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_credits_scroll_23976p_50s | timing_continuity_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_dark_2stops_23976p_3min | no false lock (C2) | PASS | locks 0 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 36; none |
| cadence | tos_dark_2stops_23976p_3min | pulldown_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_dark_2stops_23976p_3min | cadence_class_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | tos_dark_2stops_23976p_3min | timing_continuity_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | rate_119.88 | pulldown lock (C3/C5) | PASS | expected 1/5; first lock at 239 (<= 250), locked share 1.0000, film 23.976 Hz; locks 1 unlocks 0 rephases 3; exact-repeat locks 0; transport repeats 3821; lock@239(1/5 23.976) rephase@510 rephase@1009 rephase@4506 |
| cadence | rate_119.88 | film clock (C7) | PASS | 21 samples, max spacing error 0.000 ms, 0 over 1 ms (0 outside re-phase windows), 0 non-monotonic, max early 0.000 ms |
| cadence | rate_119.88 | pulldown_fixes off | INFO | locks 10 unlocks 9 rephases 0; lock@239(1/5 23.976) unlock@500 lock@739(1/5 24.000) unlock@1014 lock@1240(1/5 24.000) unlock@1500 ... |
| cadence | rate_119.88 | cadence_class_fixes off | INFO | locks 1 unlocks 0 rephases 3; lock@239(1/5 23.976) rephase@510 rephase@1009 rephase@4506 |
| cadence | rate_119.88 | timing_continuity_fixes off | INFO | locks 1 unlocks 0 rephases 3; lock@239(1/5 23.976) rephase@510 rephase@1009 rephase@4506 |
| cadence | rate_120 | pulldown lock (C3/C5) | PASS | expected 1/5; first lock at 239 (<= 250), locked share 1.0000, film 24.000 Hz; locks 1 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 3840; lock@239(1/5 24.000) |
| cadence | rate_120 | film clock (C7) | PASS | 852 samples, max spacing error 0.000 ms, 0 over 1 ms (0 outside re-phase windows), 0 non-monotonic, max early 0.000 ms |
| cadence | rate_120 | pulldown_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@239(1/5 24.000) |
| cadence | rate_120 | cadence_class_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@239(1/5 24.000) |
| cadence | rate_120 | timing_continuity_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@239(1/5 24.000) |
| cadence | rate_23.976 | no false lock (C2) | PASS | locks 0 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 0; none |
| cadence | rate_23.976 | pulldown_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | rate_23.976 | cadence_class_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | rate_23.976 | timing_continuity_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | rate_24 | no false lock (C2) | PASS | locks 0 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 0; none |
| cadence | rate_24 | pulldown_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | rate_24 | cadence_class_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | rate_24 | timing_continuity_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | rate_25 | pulldown lock (C3/C5) | PASS | expected 24/25; first lock at 74 (<= 125), locked share 1.0000, film 24.000 Hz; locks 1 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 40; lock@74(24/25 24.000) |
| cadence | rate_25 | film clock (C7) | PASS | 828 samples, max spacing error 0.000 ms, 0 over 1 ms (0 outside re-phase windows), 0 non-monotonic, max early 0.000 ms |
| cadence | rate_25 | pulldown_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@74(24/25 24.000) |
| cadence | rate_25 | cadence_class_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@74(24/25 24.000) |
| cadence | rate_25 | timing_continuity_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@74(24/25 24.000) |
| cadence | rate_29.97 | pulldown lock (C3/C5) | PASS | expected 4/5; first lock at 59 (<= 70), locked share 1.0000, film 23.976 Hz; locks 1 unlocks 0 rephases 4; exact-repeat locks 0; transport repeats 239; lock@59(4/5 23.976) rephase@256 rephase@507 rephase@758 rephase@1009 |
| cadence | rate_29.97 | film clock (C7) | PASS | 500 samples, max spacing error 0.000 ms, 0 over 1 ms (0 outside re-phase windows), 0 non-monotonic, max early 0.000 ms |
| cadence | rate_29.97 | pulldown_fixes off | INFO | locks 5 unlocks 4 rephases 0; lock@59(4/5 23.976) unlock@261 lock@307(4/5 23.976) unlock@512 lock@558(4/5 23.976) unlock@763 ... |
| cadence | rate_29.97 | cadence_class_fixes off | INFO | locks 1 unlocks 0 rephases 4; lock@59(4/5 23.976) rephase@256 rephase@507 rephase@758 rephase@1009 |
| cadence | rate_29.97 | timing_continuity_fixes off | INFO | locks 1 unlocks 0 rephases 4; lock@59(4/5 23.976) rephase@256 rephase@507 rephase@758 rephase@1009 |
| cadence | rate_30 | pulldown lock (C3/C5) | PASS | expected 4/5; first lock at 59 (<= 70), locked share 1.0000, film 24.000 Hz; locks 1 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 240; lock@59(4/5 24.000) |
| cadence | rate_30 | film clock (C7) | PASS | 852 samples, max spacing error 0.000 ms, 0 over 1 ms (0 outside re-phase windows), 0 non-monotonic, max early 0.000 ms |
| cadence | rate_30 | pulldown_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@59(4/5 24.000) |
| cadence | rate_30 | cadence_class_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@59(4/5 24.000) |
| cadence | rate_30 | timing_continuity_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@59(4/5 24.000) |
| cadence | rate_48 | pulldown lock (C3/C5) | PASS | expected 1/2; first lock at 95 (<= 100), locked share 1.0000, film 24.000 Hz; locks 1 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 960; lock@95(1/2 24.000) |
| cadence | rate_48 | film clock (C7) | PASS | 852 samples, max spacing error 0.000 ms, 0 over 1 ms (0 outside re-phase windows), 0 non-monotonic, max early 0.000 ms |
| cadence | rate_48 | pulldown_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@95(1/2 24.000) |
| cadence | rate_48 | cadence_class_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@95(1/2 24.000) |
| cadence | rate_48 | timing_continuity_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@95(1/2 24.000) |
| cadence | rate_50 | pulldown lock (C3/C5) | PASS | expected 12/25; first lock at 99 (<= 150), locked share 1.0000, film 24.000 Hz; locks 1 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 1040; lock@99(12/25 24.000) |
| cadence | rate_50 | film clock (C7) | PASS | 852 samples, max spacing error 0.000 ms, 0 over 1 ms (0 outside re-phase windows), 0 non-monotonic, max early 0.000 ms |
| cadence | rate_50 | pulldown_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@99(12/25 24.000) |
| cadence | rate_50 | cadence_class_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@99(12/25 24.000) |
| cadence | rate_50 | timing_continuity_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@99(12/25 24.000) |
| cadence | rate_59.94 | pulldown lock (C3/C5) | PASS | expected 2/5; first lock at 119 (<= 130), locked share 1.0000, film 23.976 Hz; locks 1 unlocks 0 rephases 2; exact-repeat locks 0; transport repeats 1433; lock@119(2/5 23.976) rephase@507 rephase@1009 |
| cadence | rate_59.94 | film clock (C7) | PASS | 367 samples, max spacing error 0.000 ms, 0 over 1 ms (0 outside re-phase windows), 0 non-monotonic, max early 0.000 ms |
| cadence | rate_59.94 | pulldown_fixes off | INFO | locks 5 unlocks 4 rephases 0; lock@119(2/5 23.976) unlock@512 lock@618(2/5 23.976) unlock@1014 lock@1120(2/5 23.976) unlock@1506 ... |
| cadence | rate_59.94 | cadence_class_fixes off | INFO | locks 1 unlocks 0 rephases 2; lock@119(2/5 23.976) rephase@507 rephase@1009 |
| cadence | rate_59.94 | timing_continuity_fixes off | INFO | locks 1 unlocks 0 rephases 2; lock@119(2/5 23.976) rephase@507 rephase@1009 |
| cadence | rate_60 | pulldown lock (C3/C5) | PASS | expected 2/5; first lock at 119 (<= 130), locked share 1.0000, film 24.000 Hz; locks 1 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 1440; lock@119(2/5 24.000) |
| cadence | rate_60 | film clock (C7) | PASS | 852 samples, max spacing error 0.000 ms, 0 over 1 ms (0 outside re-phase windows), 0 non-monotonic, max early 0.000 ms |
| cadence | rate_60 | pulldown_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@119(2/5 24.000) |
| cadence | rate_60 | cadence_class_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@119(2/5 24.000) |
| cadence | rate_60 | timing_continuity_fixes off | INFO | locks 1 unlocks 0 rephases 0; lock@119(2/5 24.000) |
| cadence | cad_mixed_30p_overlay_on_24p_5994p | no lock (mixed cadence) | PASS | locks 0 unlocks 0 rephases 0; exact-repeat locks 0; transport repeats 1078; none |
| cadence | cad_mixed_30p_overlay_on_24p_5994p | pulldown_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| cadence | cad_mixed_30p_overlay_on_24p_5994p | cadence_class_fixes off | INFO | locks 1 unlocks 0 rephases 7; lock@119(7/10 41.958) rephase@519 rephase@1011 rephase@1513 rephase@2015 rephase@2517 ... |
| cadence | cad_mixed_30p_overlay_on_24p_5994p | timing_continuity_fixes off | INFO | locks 0 unlocks 0 rephases 0; none |
| splice | cad_32_30p_32_30p_5994p | re-lock per segment | PASS | [seg 0 3:2: lock +119 (possible +119, <= 125), share 1.000] [seg 1 2:2(30p): lock +422 (possible +422, <= 425), share 1.000] [seg 2 3:2: lock +118 (possible +118, <= 124), share 1.000] [seg 3 2:2(30p): lock +246 (possible +246, <= 249), share 1.000]  lock@119(2/5 23.976) unlock@938 lock@1322(1/2 29.970) rephase@1807 rephase@1812 unlock@1815 lock@1918(2/5 23.976) unlock@2718 lock@2946(1/2 29.970) |
| splice | cad_32_30p_32_30p_5994p | pulldown_fixes off | INFO | [seg 0 3:2: lock +119 (possible +119, <= 125), share 1.000] [seg 1 2:2(30p): lock +422 (possible +422, <= 425), share 1.000] [seg 2 3:2: lock +118 (possible +118, <= 124), share 1.000] [seg 3 2:2(30p): lock +246 (possible +246, <= 249), share 1.000]  |
| splice | cad_32_30p_32_30p_5994p | cadence_class_fixes off | INFO | [seg 0 3:2: lock +119 (possible +119, <= 125), share 1.000] [seg 1 2:2(30p): lock +422 (possible +422, <= 425), share 1.000] [seg 2 3:2: lock +118 (possible +118, <= 124), share 1.000] [seg 3 2:2(30p): lock +246 (possible +246, <= 249), share 1.000]  |
| splice | cad_32_30p_32_30p_5994p | timing_continuity_fixes off | INFO | [seg 0 3:2: lock +119 (possible +119, <= 125), share 1.000] [seg 1 2:2(30p): lock +422 (possible +422, <= 425), share 1.000] [seg 2 3:2: lock +118 (possible +118, <= 124), share 1.000] [seg 3 2:2(30p): lock +246 (possible +246, <= 249), share 1.000]  |
| splice | cad_32_phase_jumps_5994p | re-phase in place at each break (D3) | PASS | [600: rephase +8] [1200: rephase +7] [1800: rephase +8] [2400: rephase +7] [3000: rephase +8] locked share 1.0000; lock@119(2/5 23.976) rephase@608 rephase@1207 rephase@1808 rephase@2407 rephase@3008 |
| splice | cad_32_phase_jumps_5994p | pulldown_fixes off | INFO | [600: unlock/relock +118] [1200: unlock/relock +118] [1800: unlock/relock +118] [2400: unlock/relock +118] [3000: unlock/relock +118] locked share 0.8486; lock@119(2/5 23.976) unlock@613 lock@718(2/5 23.976) unlock@1212 lock@1318(2/5 23.976) unlock@1813 lock@1918(2/5 23.976) unlock@2412 ... |
| splice | cad_32_phase_jumps_5994p | cadence_class_fixes off | INFO | [600: rephase +8] [1200: rephase +7] [1800: rephase +8] [2400: rephase +7] [3000: rephase +8] locked share 1.0000; lock@119(2/5 23.976) rephase@608 rephase@1207 rephase@1808 rephase@2407 rephase@3008 |
| splice | cad_32_phase_jumps_5994p | timing_continuity_fixes off | INFO | [600: rephase +8] [1200: rephase +7] [1800: rephase +8] [2400: rephase +7] [3000: rephase +8] locked share 1.0000; lock@119(2/5 23.976) rephase@608 rephase@1207 rephase@1808 rephase@2407 rephase@3008 |
| splice | cad_22_50p_phase_breaks | re-phase in place at each break (D3) | PASS | [302: rephase +3] [601: rephase +3] [902: rephase +3] [1201: rephase +3] locked share 1.0000; lock@99(1/2 25.000) rephase@305 rephase@604 rephase@905 rephase@1204 |
| splice | cad_22_50p_phase_breaks | pulldown_fixes off | INFO | [302: unlock/relock +99] [601: unlock/relock +100] [902: unlock/relock +99] [1201: unlock/relock +100] locked share 0.7302; lock@99(1/2 25.000) unlock@307 lock@401(1/2 25.000) unlock@606 lock@701(1/2 25.000) unlock@907 lock@1001(1/2 25.000) unlock@1206 ... |
| splice | cad_22_50p_phase_breaks | cadence_class_fixes off | INFO | [302: rephase +3] [601: rephase +3] [902: rephase +3] [1201: rephase +3] locked share 1.0000; lock@99(1/2 25.000) rephase@305 rephase@604 rephase@905 rephase@1204 |
| splice | cad_22_50p_phase_breaks | timing_continuity_fixes off | INFO | [302: rephase +3] [601: rephase +3] [902: rephase +3] [1201: rephase +3] locked share 1.0000; lock@99(1/2 25.000) rephase@305 rephase@604 rephase@905 rephase@1204 |
| classifier | cosmos_23976p_3min | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.0s; last candidate film (film-like measured rate) pan 0.740 hps wide 0.65 window 10.0 s motion 1 |
| classifier | cosmos_23976p_3min | cadence_class_fixes off | INFO | final film; unknown@0.0s film@5.0s |
| classifier | cosmos_32_5994i_3min | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.0s; last candidate film (film-like measured rate) pan 0.614 hps wide 0.51 window 10.0 s motion 1 |
| classifier | cosmos_32_5994i_3min | cadence_class_fixes off | INFO | final film; unknown@0.0s film@5.0s |
| classifier | cosmos_32_5994p_3min | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.2s; last candidate film (pulldown lock at a film rate) pan 0.734 hps wide 0.64 window 10.0 s motion 1 |
| classifier | cosmos_32_5994p_3min | cadence_class_fixes off | INFO | final film; unknown@0.0s film@7.0s |
| classifier | cosmos_longtake_23976p_44s | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.0s; last candidate film (film-like measured rate) pan 0.878 hps wide 0.79 window 10.0 s motion 1 |
| classifier | cosmos_longtake_23976p_44s | cadence_class_fixes off | INFO | final film; unknown@0.0s film@5.0s |
| classifier | meridian_2997p_3min | class film/sports | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.0s; last candidate film (film-like measured rate) pan 0.069 hps wide 0.07 window 10.0 s motion 1 |
| classifier | meridian_2997p_3min | cadence_class_fixes off | INFO | final film; unknown@0.0s film@5.0s |
| classifier | meridian_5994p_3min | class video/sports | PASS | final video, share of expected 1.000 (dominant video); unknown@0.0s video@5.0s; last candidate video (video-class source) pan 0.114 hps wide 0.06 window 10.0 s motion 1 |
| classifier | meridian_5994p_3min | cadence_class_fixes off | INFO | final video; unknown@0.0s video@5.0s |
| classifier | tos_22_50i_psf_3min | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.0s; last candidate film (film-like measured rate) pan 0.517 hps wide 0.32 window 10.0 s motion 1 |
| classifier | tos_22_50i_psf_3min | cadence_class_fixes off | INFO | final film; unknown@0.0s film@5.0s |
| classifier | tos_22_50p_3min | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.2s; last candidate film (pulldown lock at a film rate) pan 0.523 hps wide 0.32 window 10.0 s motion 1 |
| classifier | tos_22_50p_3min | cadence_class_fixes off | INFO | final film; unknown@0.0s film@7.0s |
| classifier | tos_23976p_3min | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.0s; last candidate film (film-like measured rate) pan 0.487 hps wide 0.29 window 10.0 s motion 1 |
| classifier | tos_23976p_3min | cadence_class_fixes off | INFO | final film; unknown@0.0s film@5.0s |
| classifier | tos_24p_3min | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.0s; last candidate film (film-like measured rate) pan 0.480 hps wide 0.29 window 10.0 s motion 1 |
| classifier | tos_24p_3min | cadence_class_fixes off | INFO | final film; unknown@0.0s film@5.0s |
| classifier | tos_25p_speedup_3min | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.0s; last candidate film (film-like measured rate) pan 0.517 hps wide 0.32 window 10.0 s motion 1 |
| classifier | tos_25p_speedup_3min | cadence_class_fixes off | INFO | final film; unknown@0.0s film@5.0s |
| classifier | tos_32_5994i_3min | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.0s; last candidate film (film-like measured rate) pan 0.489 hps wide 0.26 window 10.0 s motion 1 |
| classifier | tos_32_5994i_3min | cadence_class_fixes off | INFO | final film; unknown@0.0s film@5.0s |
| classifier | tos_32_5994p_3min | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.2s; last candidate film (pulldown lock at a film rate) pan 0.477 hps wide 0.29 window 10.0 s motion 1 |
| classifier | tos_32_5994p_3min | cadence_class_fixes off | INFO | final film; unknown@0.0s film@7.0s |
| classifier | tos_credits_scroll_23976p_50s | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.0s; last candidate film (film-like measured rate) pan 0.823 hps wide 0.69 window 10.0 s motion 1 |
| classifier | tos_credits_scroll_23976p_50s | cadence_class_fixes off | INFO | final film; unknown@0.0s film@5.0s |
| classifier | tos_dark_2stops_23976p_3min | class film (dark film) | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.0s; last candidate film (film-like measured rate) pan 0.471 hps wide 0.28 window 10.0 s motion 1 |
| classifier | tos_dark_2stops_23976p_3min | cadence_class_fixes off | INFO | final film; unknown@0.0s film@5.0s |
| classifier | rate_25 | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.0s; last candidate film (pulldown lock at a film rate) pan 1.249 hps wide 0.82 window 10.0 s motion 1 |
| classifier | rate_25 | cadence_class_fixes off | INFO | final film; unknown@0.0s film@5.0s |
| classifier | rate_59.94 | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.2s; last candidate film (pulldown lock at a film rate) pan 1.221 hps wide 0.82 window 10.0 s motion 1 |
| classifier | rate_59.94 | cadence_class_fixes off | INFO | final film; unknown@0.0s film@7.0s |
| classifier | cad_22_50p_phase_breaks | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.2s; last candidate film (pulldown lock at a film rate) pan 0.015 hps wide 0.00 window 10.0 s motion 1 |
| classifier | cad_22_50p_phase_breaks | cadence_class_fixes off | INFO | final film; unknown@0.0s film@7.0s |
| classifier | cad_32_30p_32_30p_5994p | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.2s; last candidate film (pulldown lock at a film rate) pan 0.029 hps wide 0.00 window 10.0 s motion 1 |
| classifier | cad_32_30p_32_30p_5994p | cadence_class_fixes off | INFO | final film; unknown@0.0s film@7.0s |
| classifier | cad_32_phase_jumps_5994p | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.2s; last candidate film (pulldown lock at a film rate) pan 0.030 hps wide 0.00 window 10.0 s motion 1 |
| classifier | cad_32_phase_jumps_5994p | cadence_class_fixes off | INFO | final film; unknown@0.0s film@7.0s |
| classifier | cad_mixed_30p_overlay_on_24p_5994p | class video | PASS | final video, share of expected 1.000 (dominant video); unknown@0.0s video@5.9s; last candidate video (video-class source) pan 0.512 hps wide 0.11 window 10.0 s motion 1 |
| classifier | cad_mixed_30p_overlay_on_24p_5994p | cadence_class_fixes off | INFO | final film; unknown@0.0s film@7.0s |
| classifier | drift_10min_23976p_lowbitrate | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.0s; last candidate film (film-like measured rate) pan 0.405 hps wide 0.24 window 10.0 s motion 1 |
| classifier | drift_10min_23976p_lowbitrate | cadence_class_fixes off | INFO | final film; unknown@0.0s film@5.0s |
| classifier | pan_120pxs_23976p | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.0s; last candidate film (film-like measured rate) pan 0.152 hps wide 0.00 window 10.0 s motion 1 |
| classifier | pan_120pxs_23976p | cadence_class_fixes off | INFO | final film; unknown@0.0s film@5.0s |
| classifier | pan_120pxs_5994p | class video | PASS | final video, share of expected 1.000 (dominant video); unknown@0.0s video@5.0s; last candidate video (video-class source) pan 0.076 hps wide 0.00 window 10.0 s motion 1 |
| classifier | pan_120pxs_5994p | cadence_class_fixes off | INFO | final video; unknown@0.0s video@5.0s |
| classifier | pan_1440pxs_23976p | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.0s; last candidate film (film-like measured rate) pan 1.811 hps wide 0.99 window 10.0 s motion 1 |
| classifier | pan_1440pxs_23976p | cadence_class_fixes off | INFO | final film; unknown@0.0s film@5.0s |
| classifier | pan_1440pxs_5994p | class video (candidate sports) | PASS | final video, share of expected 1.000 (dominant video); unknown@0.0s video@5.0s; last candidate sports (video-class source with sustained large pans) pan 1.729 hps wide 1.00 window 10.0 s motion 1 |
| classifier | pan_1440pxs_5994p | cadence_class_fixes off | INFO | final video; unknown@0.0s video@5.0s |
| classifier | pan_240pxs_dark_lowcontrast_23976p | class film (dark film) | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.0s; last candidate film (film-like measured rate) pan 0.299 hps wide 1.00 window 10.0 s motion 1 |
| classifier | pan_240pxs_dark_lowcontrast_23976p | cadence_class_fixes off | INFO | final film; unknown@0.0s film@5.0s |
| classifier | pan_240pxs_dark_lowcontrast_5994p | class video (candidate sports) (dark film) | PASS | final video, share of expected 1.000 (dominant video); unknown@0.0s video@5.0s; last candidate sports (video-class source with sustained large pans) pan 0.246 hps wide 0.32 window 10.0 s motion 1 |
| classifier | pan_240pxs_dark_lowcontrast_5994p | cadence_class_fixes off | INFO | final video; unknown@0.0s video@5.0s |
| classifier | pan_480pxs_23976p | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.0s; last candidate film (film-like measured rate) pan 0.598 hps wide 0.99 window 10.0 s motion 1 |
| classifier | pan_480pxs_23976p | cadence_class_fixes off | INFO | final film; unknown@0.0s film@5.0s |
| classifier | pan_480pxs_5994p | class video (candidate sports) | PASS | final video, share of expected 1.000 (dominant video); unknown@0.0s video@5.0s; last candidate sports (video-class source with sustained large pans) pan 0.675 hps wide 1.00 window 10.0 s motion 1 |
| classifier | pan_480pxs_5994p | cadence_class_fixes off | INFO | final video; unknown@0.0s video@5.0s |
| classifier | pan_480pxs_fade_23976p | class film | PASS | final film, share of expected 1.000 (dominant film); unknown@0.0s film@5.0s; last candidate film (film-like measured rate) pan 1.025 hps wide 1.00 window 10.0 s motion 1 |
| classifier | pan_480pxs_fade_23976p | cadence_class_fixes off | INFO | final film; unknown@0.0s film@5.0s |
| classifier | pan_480pxs_fade_5994p | class video (candidate sports) | PASS | final video, share of expected 1.000 (dominant video); unknown@0.0s video@5.0s; last candidate sports (video-class source with sustained large pans) pan 1.153 hps wide 1.00 window 10.0 s motion 1 |
| classifier | pan_480pxs_fade_5994p | cadence_class_fixes off | INFO | final video; unknown@0.0s video@5.0s |
| discontinuity | dis_dropped_frames_23976_vfr | missed frames flagged at the logged frames | PASS | expected 240,599,775,997,1196; missed 240,599,775,997,1196; breaks -; invalidated 240,599,775,997,1196; gap pairs 240,599,775,997,1196; refused -; timeline restarts -; matcher restarts 0 |
| discontinuity | dis_dropped_frames_23976_vfr | pulldown_fixes off | INFO | expected 240,599,775,997,1196; missed 240,599,775,997,1196; breaks -; invalidated 240,599,775,997,1196; gap pairs 240,599,775,997,1196; refused -; timeline restarts -; matcher restarts 0 |
| discontinuity | dis_dropped_frames_23976_vfr | cadence_class_fixes off | INFO | expected 240,599,775,997,1196; missed 240,599,775,997,1196; breaks -; invalidated 240,599,775,997,1196; gap pairs 240,599,775,997,1196; refused -; timeline restarts -; matcher restarts 0 |
| discontinuity | dis_dropped_frames_23976_vfr | timing_continuity_fixes off | INFO | expected 240,599,775,997,1196; missed 240,599,775,997,1196; breaks -; invalidated 240,599,775,997,1196; gap pairs 240,599,775,997,1196; refused -; timeline restarts -; matcher restarts 0 |
| discontinuity | dis_duplicated_frames_23976 | duplicates seen as repeats at the logged frames, no gap, no lock | PASS | expected 241,602,780,1004,1005,1206; found 6/6; other repeat flags 0 (static content); gaps 0; locks 0; invalidations 0 |
| discontinuity | dis_gap_black_2500ms_23976 | no timing event, the repeats never lock | PASS | gaps 0; locks 0; none; invalidations 0; timeline restarts 0; SR cuts 2.00,25.02,25.53,28.53,46.05,49.05 |
| discontinuity | dis_gap_frozen_2500ms_23976 | no timing event, the repeats never lock | PASS | gaps 0; locks 0; none; invalidations 0; timeline restarts 0; SR cuts 2.00,25.02,25.53,28.53,46.05,49.05 |
| discontinuity | dis_gap_pts_2500ms_23976 | timestamp gap: one break, vectors invalidated, no pair across | PASS | expected 720; breaks 720; missed -; invalidated 720; gap pairs -; timeline restarts 720 |
| discontinuity | dis_gap_pts_2500ms_23976 | pulldown_fixes off | INFO | expected 720; breaks 720; missed -; invalidated 720; gap pairs -; timeline restarts 720 |
| discontinuity | dis_gap_pts_2500ms_23976 | cadence_class_fixes off | INFO | expected 720; breaks 720; missed -; invalidated 720; gap pairs -; timeline restarts 720 |
| discontinuity | dis_gap_pts_2500ms_23976 | timing_continuity_fixes off | INFO | expected 720; breaks 720; missed -; invalidated -; gap pairs -; timeline restarts 720 |
| discontinuity | dis_vfr_jitter_2ms_23976 | +-2 ms jitter: no false event | PASS | gaps 0, invalidations 0, timeline restarts 0, re-anchors 0, adjacency refusals 0, locks 0 |
| discontinuity | dis_vfr_jitter_2ms_23976 | pulldown_fixes off | INFO | gaps 0, invalidations 0, timeline restarts 0, re-anchors 0, adjacency refusals 0, locks 0 |
| discontinuity | dis_vfr_jitter_2ms_23976 | cadence_class_fixes off | INFO | gaps 0, invalidations 0, timeline restarts 0, re-anchors 0, adjacency refusals 0, locks 0 |
| discontinuity | dis_vfr_jitter_2ms_23976 | timing_continuity_fixes off | INFO | gaps 0, invalidations 0, timeline restarts 0, re-anchors 0, adjacency refusals 0, locks 0 |
| discontinuity | dis_ratechange_23976_to_5994 | capture: one mode change at the logged frame | PASS | expected 720; resync 720; breaks 0; missed 0; locks 0; class unknown@0.0s film@5.0s unknown@30.0s video@35.0s |
| discontinuity | dis_ratechange_23976_to_5994@player59.94 | played at 59.94: 3:2 lock, lost within 1 s of the change, no re-lock | PASS | change at signal 1800; first lock 119; unlock 1805 (+5); re-lock after -1; lock@119(2/5 23.976) unlock@1805; class unknown@-0.0s film@5.2s video@45.7s |
| motion | pan_120pxs_23976p | pan: no event, vectors valid | PASS | gaps 0, invalidations 0, timeline restarts 0, locks 0, pairs 718 refused 0, repeats 0 |
| motion | pan_120pxs_5994p | pan: no event, vectors valid | PASS | gaps 0, invalidations 0, timeline restarts 0, locks 0, pairs 1797 refused 0, repeats 0 |
| motion | pan_1440pxs_23976p | pan: no event, vectors valid | PASS | gaps 0, invalidations 0, timeline restarts 0, locks 0, pairs 718 refused 0, repeats 0 |
| motion | pan_1440pxs_5994p | pan: no event, vectors valid | PASS | gaps 0, invalidations 0, timeline restarts 0, locks 0, pairs 1797 refused 0, repeats 1 |
| motion | pan_240pxs_dark_lowcontrast_23976p | pan: no event, vectors valid | PASS | gaps 0, invalidations 0, timeline restarts 0, locks 0, pairs 718 refused 0, repeats 0 |
| motion | pan_240pxs_dark_lowcontrast_5994p | pan: no event, vectors valid | PASS | gaps 0, invalidations 0, timeline restarts 0, locks 0, pairs 1797 refused 0, repeats 2 |
| motion | pan_480pxs_23976p | pan: no event, vectors valid | PASS | gaps 0, invalidations 0, timeline restarts 0, locks 0, pairs 718 refused 0, repeats 1 |
| motion | pan_480pxs_5994p | pan: no event, vectors valid | PASS | gaps 0, invalidations 0, timeline restarts 0, locks 0, pairs 1797 refused 0, repeats 2 |
| motion | pan_480pxs_fade_23976p | pan with fades: no event, no cut | PASS | gaps 0, invalidations 0, timeline restarts 0, locks 0, pairs 718 refused 0, repeats 1; SR cuts on the fades: - (latch fixes off: -) |
| motion | pan_480pxs_fade_5994p | pan with fades: no event, no cut | PASS | gaps 0, invalidations 0, timeline restarts 0, locks 0, pairs 1797 refused 0, repeats 10; SR cuts on the fades: - (latch fixes off: -) |
| source-resolution | cosmos_23976p_3min | cuts found, native never engaged | PASS | truth 16, found 67, matched 14 (recall 0.88), unlisted 51; missed 24.25,29.00; latch fixes off: found 10 matched 6; engages 0; declared 34.0s->0.35 58.1s->1.00; readings native 351 lower 9 blank 0 |
| source-resolution | cosmos_32_5994i_3min | cuts found, native never engaged | PASS | truth 16, found 67, matched 14 (recall 0.88), unlisted 51; missed 24.25,29.00; latch fixes off: found 10 matched 6; engages 0; declared 31.0s->0.50 34.0s->1.00 129.6s->0.75 138.6s->1.00; readings native 345 lower 15 blank 0 |
| source-resolution | cosmos_32_5994p_3min | cuts found, native never engaged | PASS | truth 16, found 66, matched 14 (recall 0.88), unlisted 50; missed 24.25,29.00; latch fixes off: found 10 matched 6; engages 0; declared 33.5s->0.50 34.5s->1.00 131.1s->0.50 139.1s->1.00; readings native 340 lower 20 blank 0 |
| source-resolution | cosmos_longtake_23976p_44s | cut-free shot: native never engaged (cuts reported) | PASS | cuts 1.50,28.53,31.03,34.53,35.03 (latch fixes off 34.53,35.03); engages 0; declared never; native 89 lower 0 blank 0 |
| source-resolution | meridian_2997p_3min | cuts found, native never engaged | PASS | truth 19, found 26, matched 18 (recall 0.95), unlisted 7; missed 170.08; latch fixes off: found 6 matched 5; engages 0; declared never; readings native 357 lower 3 blank 0 |
| source-resolution | meridian_5994p_3min | cuts found, native never engaged | PASS | truth 19, found 26, matched 18 (recall 0.95), unlisted 7; missed 170.08; latch fixes off: found 6 matched 5; engages 0; declared never; readings native 358 lower 2 blank 0 |
| source-resolution | tos_22_50i_psf_3min | cuts found, native never engaged | PASS | truth 15, found 42, matched 13 (recall 0.87), unlisted 27; missed 10.76,65.28; latch fixes off: found 15 matched 9; engages 0; declared never; readings native 317 lower 14 blank 2 |
| source-resolution | tos_22_50p_3min | cuts found, native never engaged | PASS | truth 15, found 34, matched 10 (recall 0.67), unlisted 23; missed 7.12,10.76,65.28,74.52,81.16; latch fixes off: found 15 matched 8; engages 0; declared 50.9s->0.75 66.5s->1.00 93.0s->0.75 97.7s->1.00; readings native 320 lower 11 blank 2 |
| source-resolution | tos_23976p_3min | cuts found, native never engaged | PASS | truth 15, found 34, matched 11 (recall 0.73), unlisted 22; missed 7.42,11.22,68.07,84.63; latch fixes off: found 13 matched 7; engages 0; declared 69.1s->0.75 69.6s->1.00 100.6s->0.75 101.1s->1.00; readings native 344 lower 14 blank 2 |
| source-resolution | tos_24p_3min | cuts found, native never engaged | PASS | truth 15, found 34, matched 11 (recall 0.73), unlisted 22; missed 7.42,11.21,68.00,84.54; latch fixes off: found 13 matched 7; engages 0; declared 69.0s->0.75 69.5s->1.00 100.5s->0.75 111.0s->1.00; readings native 344 lower 14 blank 2 |
| source-resolution | tos_25p_speedup_3min | cuts found, native never engaged | PASS | truth 15, found 40, matched 12 (recall 0.80), unlisted 26; missed 10.76,65.28,81.16; latch fixes off: found 15 matched 9; engages 0; declared 96.7s->0.75 97.2s->1.00; readings native 319 lower 12 blank 2 |
| source-resolution | tos_32_5994i_3min | cuts found, native never engaged | PASS | truth 15, found 28, matched 11 (recall 0.73), unlisted 16; missed 7.42,11.22,68.07,84.63; latch fixes off: found 12 matched 6; engages 0; declared 69.1s->0.75 97.1s->1.00; readings native 345 lower 12 blank 3 |
| source-resolution | tos_32_5994p_3min | cuts found, native never engaged | PASS | truth 15, found 34, matched 11 (recall 0.73), unlisted 22; missed 7.42,11.22,68.07,84.63; latch fixes off: found 13 matched 7; engages 0; declared 100.6s->0.75 101.1s->1.00; readings native 346 lower 12 blank 2 |
| source-resolution | tos_credits_scroll_23976p_50s | cut-free shot: native never engaged (cuts reported) | PASS | cuts 2.00,25.02,25.53,28.53,46.05,49.05 (latch fixes off -); engages 0; declared never; native 99 lower 1 blank 0 |
| source-resolution | tos_dark_2stops_23976p_3min | cuts found, native never engaged | PASS | truth 15, found 35, matched 12 (recall 0.80), unlisted 22; missed 7.42,11.22,68.07; latch fixes off: found 5 matched 3; engages 0; declared never; readings native 336 lower 21 blank 3 |
| source-resolution | cosmos_longtake_23976p_44s@upscale2x | long take crossfades after the wait | KNOWN BUG (round 3) | declared 10.01 s (expect ~10), engaged 28.65 s (expect 30.11) by 'engaged at a cut', fade 0.00 s, ratio 0.50; cuts 1.50,28.53,31.03,34.53,35.03; latch fixes off: 0 changes, declared 35.03 |
| source-resolution | cosmos_longtake_23976p_44s@upscale2x | evidence standard kept at the stream start (10 s in one shot) | PASS | declared 10.01 s |
| source-resolution | cosmos_longtake_23976p_44s@flashes | flashes add no cut | PASS | cuts 1.50,28.53,31.03,34.53,35.03 vs 1.50,28.53,31.03,34.53,35.03 without the flashes (latch fixes off: 10.01,10.51,20.02,20.52,30.03,30.53,34.53,35.03 vs 34.53,35.03) |
| source-resolution | tos_24p_3min@upscale2x | upscale engages (at a cut / black / crossfade) at ~0.5 | PASS | declared 1.50 s, engaged 18.12 s by 'engaged at a cut' (0.42 s after a true cut), ratio 0.500, changes 6; latch fixes off: changes 2 |
| player-pulldown | tos_23976p_3min@player59.94 | 2/5 lock, film clock | PASS | first lock 119 (<= 130), share 1.0000, unlocks 0, rephases 0, clock max spacing error 0.000 ms (0 over 1 ms); class unknown@-0.0s film@5.2s |
| player-pulldown | tos_24p_3min@player60.00 | 2/5 lock, film clock | PASS | first lock 119 (<= 130), share 1.0000, unlocks 0, rephases 0, clock max spacing error 0.000 ms (0 over 1 ms); class unknown@-0.0s film@5.2s |
| player-pulldown | tos_25p_speedup_3min@player50.00 | 1/2 lock, film clock | PASS | first lock 99 (<= 104), share 1.0000, unlocks 0, rephases 0, clock max spacing error 0.000 ms (0 over 1 ms); class unknown@-0.0s film@5.2s |
| player-pulldown | tos_dark_2stops_23976p_3min@player59.94 | 2/5 lock, film clock | PASS | first lock 119 (<= 130), share 1.0000, unlocks 0, rephases 0, clock max spacing error 0.000 ms (0 over 1 ms); class unknown@-0.0s film@5.2s |
| player-pulldown | cosmos_23976p_3min@player59.94 | 2/5 lock, film clock | PASS | first lock 119 (<= 130), share 1.0000, unlocks 0, rephases 0, clock max spacing error 0.000 ms (0 over 1 ms); class unknown@-0.0s film@5.2s |
| player-pulldown | cosmos_longtake_23976p_44s@player59.94 | 2/5 lock, film clock | PASS | first lock 119 (<= 130), share 1.0000, unlocks 0, rephases 0, clock max spacing error 0.000 ms (0 over 1 ms); class unknown@-0.0s film@5.2s |
| player-pulldown | meridian_2997p_3min@player59.94 | 1/2 lock, film clock | PASS | first lock 119 (<= 124), share 1.0000, unlocks 0, rephases 0, clock max spacing error 0.000 ms (0 over 1 ms); class unknown@-0.0s film@5.2s |
| player-pulldown | meridian_5994p_3min@player59.94 | no lock (video at the signal rate) | PASS | locks 0; none |
| drift | drift_10min_23976p_lowbitrate | no flush at 59.940 Hz display | PASS | [+0 ppm: flushes 0 holds 55 settled holds 55 max depth 5] [+100 ppm: flushes 0 holds 53 settled holds 53 max depth 6] [-100 ppm: flushes 0 holds 56 settled holds 56 max depth 5] queue_timing_fixes off (0 ppm): flushes 0, first -1.0 s, median interval 0.0 s |
| drift | drift_10min_23976p_lowbitrate | no flush at 60.000 Hz display | PASS | [+0 ppm: flushes 0 holds 55 settled holds 55 max depth 5] [+100 ppm: flushes 0 holds 53 settled holds 53 max depth 6] [-100 ppm: flushes 0 holds 56 settled holds 56 max depth 5] queue_timing_fixes off (0 ppm): flushes 0, first -1.0 s, median interval 0.0 s |
| drift | drift_10min_23976p_lowbitrate | no flush at 50.000 Hz display | PASS | [+0 ppm: flushes 0 holds 45 settled holds 45 max depth 5] [+100 ppm: flushes 0 holds 44 settled holds 44 max depth 6] [-100 ppm: flushes 0 holds 46 settled holds 46 max depth 5] queue_timing_fixes off (0 ppm): flushes 0, first -1.0 s, median interval 0.0 s |
| drift | drift_10min_23976p_lowbitrate | no flush at 23.976 Hz display | PASS | [+0 ppm: flushes 0 holds 22 settled holds 22 max depth 6] [+100 ppm: flushes 0 holds 22 settled holds 22 max depth 6] [-100 ppm: flushes 0 holds 22 settled holds 22 max depth 6] queue_timing_fixes off (0 ppm): flushes 4, first 0.3 s, median interval 151.0 s |
| drift | drift_10min_23976p_lowbitrate | no flush at 120.000 Hz display | PASS | [+0 ppm: flushes 0 holds 109 settled holds 109 max depth 5] [+100 ppm: flushes 0 holds 106 settled holds 106 max depth 6] [-100 ppm: flushes 0 holds 110 settled holds 110 max depth 5] queue_timing_fixes off (0 ppm): flushes 0, first -1.0 s, median interval 0.0 s |
| drift | drift_10min_23976p_lowbitrate@player59.94 | 3:2 held for 10 min, film clock spaced 1/f | PASS | first lock 119, share 1.0000, unlocks 0, clock max spacing error 0.000 ms over 12373 samples |
| drift | drift_10min_23976p_lowbitrate@player59.94 | no flush at 59.940 Hz display | PASS | [+0 ppm: flushes 0 holds 61 settled holds 54 max depth 6] [+100 ppm: flushes 0 holds 60 settled holds 53 max depth 6] [-100 ppm: flushes 0 holds 62 settled holds 55 max depth 6] queue_timing_fixes off (0 ppm): flushes 0, first -1.0 s, median interval 0.0 s |
| drift | drift_10min_23976p_lowbitrate@player59.94 | no flush at 60.000 Hz display | PASS | [+0 ppm: flushes 0 holds 62 settled holds 55 max depth 6] [+100 ppm: flushes 0 holds 60 settled holds 53 max depth 6] [-100 ppm: flushes 0 holds 62 settled holds 55 max depth 6] queue_timing_fixes off (0 ppm): flushes 0, first -1.0 s, median interval 0.0 s |
| drift | drift_10min_23976p_lowbitrate@player59.94 | no flush at 50.000 Hz display | PASS | [+0 ppm: flushes 0 holds 52 settled holds 46 max depth 6] [+100 ppm: flushes 0 holds 50 settled holds 44 max depth 6] [-100 ppm: flushes 0 holds 52 settled holds 46 max depth 6] queue_timing_fixes off (0 ppm): flushes 0, first -1.0 s, median interval 0.0 s |
| drift | drift_10min_23976p_lowbitrate@player59.94 | no flush at 23.976 Hz display | PASS | [+0 ppm: flushes 0 holds 25 settled holds 22 max depth 7] [+100 ppm: flushes 0 holds 25 settled holds 22 max depth 7] [-100 ppm: flushes 0 holds 25 settled holds 22 max depth 7] queue_timing_fixes off (0 ppm): flushes 8, first 0.2 s, median interval 115.3 s |
| drift | drift_10min_23976p_lowbitrate@player59.94 | no flush at 120.000 Hz display | PASS | [+0 ppm: flushes 0 holds 124 settled holds 109 max depth 5] [+100 ppm: flushes 0 holds 119 settled holds 105 max depth 6] [-100 ppm: flushes 0 holds 125 settled holds 110 max depth 5] queue_timing_fixes off (0 ppm): flushes 0, first -1.0 s, median interval 0.0 s |
| queue | tos_23976p_3min | no flush at 59.94/60/50/23.976/120 (fixes / off) | PASS | [59.940: 0/0] [60.000: 0/0] [50.000: 0/0] [23.976: 0/2] [120.000: 0/0] total 0 vs 2 off |
| queue | tos_24p_3min | no flush at 59.94/60/50/23.976/120 (fixes / off) | PASS | [59.940: 0/0] [60.000: 0/0] [50.000: 0/0] [23.976: 0/2] [120.000: 0/0] total 0 vs 2 off |
| queue | tos_25p_speedup_3min | no flush at 59.94/60/50/23.976/120 (fixes / off) | PASS | [59.940: 0/0] [60.000: 0/0] [50.000: 0/0] [23.976: 0/0] [120.000: 0/0] total 0 vs 0 off |
| queue | tos_32_5994p_3min | no flush at 59.94/60/50/23.976/120 (fixes / off) | PASS | [59.940: 0/0] [60.000: 0/0] [50.000: 0/0] [23.976: 0/3] [120.000: 0/0] total 0 vs 3 off |
| queue | tos_22_50p_3min | no flush at 59.94/60/50/23.976/120 (fixes / off) | PASS | [59.940: 0/0] [60.000: 0/0] [50.000: 0/1] [23.976: 0/9] [120.000: 0/0] total 0 vs 10 off |
| queue | meridian_5994p_3min | no flush at 59.94/60/50/23.976/120 (fixes / off) | PASS | [59.940: 0/0] [60.000: 0/0] [50.000: 0/0] [23.976: 0/3] [120.000: 0/0] total 0 vs 3 off |
| queue | meridian_2997p_3min | no flush at 59.94/60/50/23.976/120 (fixes / off) | PASS | [59.940: 0/0] [60.000: 0/0] [50.000: 0/0] [23.976: 0/3] [120.000: 0/0] total 0 vs 3 off |
| queue | rate_120 | no flush at 59.94/60/50/23.976/120 (fixes / off) | PASS | [59.940: 0/0] [60.000: 0/0] [50.000: 0/0] [23.976: 0/0] [120.000: 0/0] total 0 vs 0 off |
| queue | dis_dropped_frames_23976_vfr | no flush at 59.94/60/50/23.976/120 (fixes / off) | PASS | [59.940: 0/0] [60.000: 0/0] [50.000: 0/0] [23.976: 0/1] [120.000: 0/0] total 0 vs 1 off |
| queue | dis_vfr_jitter_2ms_23976 | no flush at 59.94/60/50/23.976/120 (fixes / off) | PASS | [59.940: 0/0] [60.000: 0/0] [50.000: 0/0] [23.976: 0/1] [120.000: 0/0] total 0 vs 1 off |
