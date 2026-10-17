# Source-resolution auto: latch / unlatch review (2026-10-17)

> **Status:** document only, copied from branch `test/srcres-latch` (9afc09da). The tests
> and the F12 confidence fix it describes are on that branch and are NOT merged here.
> The owner's 2026-10-17 switching decisions (switch at the next cut / true black / fast
> pan / low-detail moment; long takes crossfade after about 20 s with a 1.5 s fade;
> release to native immediately, optional fade of at most 0.25 s) are not built yet.

Audit of `source_resolution: auto` engaging (latching) and disengaging, with a CPU
stress corpus. Tests: `src/VideoProcessor-Test/SourceResolutionLatchTests.cpp`
(real footage from `VP_TEST_REAL_FOOTAGE`, else `D:\hfr`; skipped without it).

## How the latch works (as built at 733220a3)

- **Readings** (`SourceResolution::ReadFrame`): mirror coherence per tile, averaged per
  axis; lower only when both axes reach 0.4, curve floor <= 0.35, few foreign peaks.
  Mixed-screen guard: sharp-step tiles (>= 0.01), native picture tiles, UI spread or
  a non-text-like overlay tile force native.
- **Tracker**: a 120 s window of informative frames. Down: the window's *highest*
  reading below `declared - 0.03`, with >= 3 shots (tile-mean jump > 20/255) and >= 10 s
  evidence. Up: any frame above `declared + 0.03` at once. A native reading counts only
  if its relative detail above the rate beats the window's median by 0.07 decades
  (else "starved"). F12: 6 lower frames in the same shot after native frames with a
  0.3-decade detail drop trim the window; 10 s alone then declares.
- **Monitor**: tiles every 0.5 s (wall clock), confidence = evidence x clarity
  (mean min-axis coherence over 120 s, (c - 0.4) / 0.3); auto needs 0.8.
- **AutoFactor**: down waits for the next tracker cut or a blank analysed frame, or
  the look-ahead's verified cut/black frame; up on detail is immediate; a pure
  confidence drop waits for a cut / black / 5 s.

## Measured (24 fps unless stated; 0.1 s readback+worker latency modelled)

| Scenario | Result |
|---|---|
| Native 4K (SVT): clean, grain 12 codes, q10 coded, defocused 4-6 px, dark | 0 engages, never declared; every frame read native (fog included) |
| UVG 4K (vertically soft), real-codec native x264/x265/AV1-FGS | 0 uniform engages |
| 1080 (lanczos3, Catmull, C0.6, C0.75, bilinear, Mitchell; q0-6, grain) / 1440 / 720 | declared at 10.0 s (cold), engaged at the next cut +0.125 s; class exact (0.5000 / 0.6667 / 0.3333); readings within 0.0003 |
| Kernel at 2:1 | lanczos3, C0.6, C0.75 identified; Catmull q3, bilinear, Mitchell, real ffmpeg: none (tied fits on soft pictures; never wrong) |
| Native -> upscale cut | engaged **124 s** after the first upscaled cut (window) |
| Upscale -> native cut, sharp or defocused native | native 0.125 s after the cut (3 frames) |
| Ladder 1080 -> 4K mid-shot | 0.100-0.125 s at 24/50/60 (3 / 5 / 6 frames) |
| Ladder 4K -> 1080 mid-shot | **was never**; fixed: 14.1 s = first cut after F12's 10 s |
| Native menu / credits over engaged upscale | unlatched 0.125 s; resumed **124 s** after the menu left |
| Subtitles, transport bar (one row) | stayed engaged (bar re-based - see R6) |
| Native PiP over upscale; upscaled PiP over native | never engaged |
| Letterbox 2.39 in-stream; change to full frame mid-shot | engaged, no flap, no spurious cut |
| 4:3 with player-added bars (VP not cropping) | **never engaged**: bar edges lift H floor to 0.53 > 0.35 |
| Fades through black | engaged at a tracker "cut" at the end of a fade-in (13 s from any real cut), not on black |
| Long dark scene (x0.12) | **one false native on its fade-in** -> unlatched, then blocked 120 s |
| Still (exact or dithered) from cold | never engaged; pause after engage: no change |
| Jitter 4 ms, 15 % lost copies, dither, 60 fps | engaged once (0.8 s after a cut, a lost copy); native never |
| Clock gap > 2 s | unlatched; logged as "detail above the re-based height" |
| 24 / 50 / 60 fps | unlatch and re-engage times equal within 0.03 s: time-based |
| Tracker, synthetic 1 h: 0.2 % false-native frames | 10 unlatches, native 45 % of the hour |

## Bug fixed

**F12 could never engage auto.** The tracker declares a mid-shot rendition drop on
time alone (one shot), but the monitor's confidence multiplied by
`Shots / minimumShots` (1/3), so confidence stayed <= 0.33 < 0.8 until two more cuts
(by then the 120 s window rule would usually apply anyway). Now
`Tracker::DeclaredByRendition()` marks such a declaration and the monitor (uniform
and per-axis) judges its evidence by `renditionSeconds` alone. Tests:
`TrackerRenditionChangeDeclaresOnTimeAlone`, `LadderSwitchMidShotAt24_50_60`.

## Findings not fixed (report first)

1. **Any native evidence blocks re-engagement for the full 120 s window**
   (`Candidate()` is the window maximum). Cold start is 3 shots / 10 s, but after a
   native shot, a 3 s menu or one false-native frame it is 120 s. Safe, but the
   owner's 10 s standard holds only from cold, and one false native costs 2 minutes.
2. **Low-contrast frames read false native.** At ~0.1 contrast the output rounding
   dominates: coherence < 0.4 while tiles stay informative, and `DetailAbove` (a
   relative, above/below measure) rises (-0.48 against a -2.1 reference). The starved
   test is not level-aware. Suggest: treat a native reading whose above-band level is
   within N decades of the code-rounding floor as starved (as the mixed guard's
   `mixedDetailFloorDecades`), validated against soft real 4K first.
3. **Player-added bars block engagement** when VP is not cropping (tiles straddle the
   bar edge; floor 0.53). Suggest leaving out tiles holding a flat bar region, or
   applying the floor test per tile and dropping high-floor tiles from the average.
4. **Fades engage mid-transition**: the tracker's cut is a 20/255 tile-mean jump
   between analyses 0.5 s apart, which a fade produces. With the look-ahead the
   per-frame check waits for the black; without it the switch lands on a fade step.
5. **Tracker-internal discontinuities** (gap > 2 s, time backwards) reset only the
   tracker: the monitor's kernel votes, coherence history and per-axis state persist,
   and the log says "detail above the re-based height". A source that pauses > 2 s
   without delivering frames resets all evidence (owner rule: pauses add nothing).
6. **One-row UI** (a transport bar with times) over an upscale is re-based (subtitle-
   like); the 4K bar is softened while shown.
7. **Declared value is the window maximum**: reading noise biases it up (synthetic
   +-0.01 -> 0.5099; real readings spread only 0.0003, so harmless today).

## Fixed constants that should be relative or time-based

- `Monitor::kTile` 256 px: frequency maths is per pixel (fine), but coverage scales
  with resolution (6 % of a 4K frame, 1.6 % at 8K) and the dense grid needs >= 1040 px
  height. Tile count or size from the picture size.
- `cutLevel` 20/255 absolute code jump: dark and HDR (PQ) cuts are missed, fades and
  flashes count. Relative to the shot's level/contrast, or a histogram distance.
- `renditionFrames` 6 (analysed frames): seconds, so a changed `intervalSeconds`
  keeps the meaning.
- `tolerance` 0.03 absolute ratio: 9 % at 720, 4.5 % at 1440; relative is uniform.
- `windowSeconds` 120 is the effective re-engage latency (finding 1);
  `maximumGapSeconds` 2, `kDisengageWaitSeconds` 5, kernel votes 6 / 2 shots / 120 s,
  confidence ramp 0.3: taste values, time-based (fine).
- Look-ahead blank spread 3/1023 assumes a 10-bit sampler (it is one).

## Recommendations

1. Keep the safety behaviour: no native engagement in any scenario, unlatch in one
   tile copy (<= 0.5 s + latency) at 24/50/60.
2. Shorten the post-native block: drop window samples older than the last native
   *shot* once N later shots agree, or expire mixed-screen frames when the UI leaves;
   keep 120 s only after real native picture.
3. Make the starved test level-aware (finding 2) before relying on auto in dark films.
4. Prefer the look-ahead path; without it, require the blank-frame engage to be a
   real black (mean near black), not only "uninformative", and do not treat a fade
   step as a cut (finding 4).
5. Per-tile floor/bar exclusion (finding 3); route tracker-internal gaps through
   `Monitor::Discontinuity` with the right reason (finding 5).
6. Apple TV kernel: not documented (G19); add it to the candidates from captures.

## Tests and cost

17 CPU tests, about 7.7 min single process (analysed frame: 95 ms for 8 tiles, 165 ms for 16).
No GPU test was needed: the CPU tiles are the pixels the GPU copy reads (nearest,
native luma), and `AnalyseTilesForTest` gained an overload with tile centres / base
count, so the dense grid and frame signature match `Monitor::Frame`.
