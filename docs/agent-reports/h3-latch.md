# h3-latch: source-resolution cut judge, engage timing and start-up crop (2026-10-18)

Branch `fix/h3-latch`, worktree `D:\codex\videoprocessor\h3-latch`, from `test/real-footage-suite` 8ceaaa33.
Local only. The crossfade mix itself (BTB-safe mix, the old path's stages) is untouched (fix/h3-picture).

Two new switches, both default on, both with a Config control and schema validation:

| Key | Default | What |
|---|---|---|
| `[vprenderer] source_resolution_cut_fixes` | on (needs `source_resolution_latch_fixes`) | items 1-4 |
| `[vprenderer] active_picture_startup_hold` | on | item 5 |

Off gives the earlier behaviour; the suite runs a "cut fixes off" lane next to the default one.

## What changed

`src/VideoProcessor-Lib/SourceResolutionDetector.h/.cpp`

- `FrameReading::distribution` (32-bin luma distribution of the pattern tiles, cpp:1103) and
  `DistributionChange` (cpp:1294).
- `Tracker::Settings::cutFixes` and its thresholds (h:482), the shared `CutRule` / `NearRule` (h:505-512).
- `Tracker::CutFixesStep` (cpp:1444): the cut judge with the cut fixes; `ConfirmCut` (cpp:1421) keeps
  each cut's own moment (`LastCutSeconds`) and moves frames analysed since a deferred cut into the new
  shot; `Dark` (cpp:1410).
- `FrameCutJudge` (h:641, cpp:1835-1925): the look-ahead's per-frame judge with the dark and flash
  rules, and `Evidence(from, to)` for the tracker (`FrameEvidence`, h:378).
- R3-2: shots also need `shotEvidenceShare` (0.5) of `minimumSeconds` (cpp:1755).

`src/VideoProcessor-Lib/vprenderer/SourceResolutionMonitor.h/.cpp`

- Jobs carry the source sequence. `Analyse` gives the trackers the look-ahead's evidence for their step
  (cpp:722).
- `LookAheadFrame` runs `FrameCutJudge` (cpp:210). A flash cancels its pending switch event (cpp:239).
- Switches use the cut moments, not the deferred confirmations (`Snapshot::moments`, cpp:871, 1154).

`src/VideoProcessor-Lib/vprenderer/StartupCropHold.h` (new) and `LibplaceboVideoRenderer.cpp`

- The hold: the decision at cpp:20057; black at present, cpp:22355 (render-ahead extras 15515/15541).
- `BeginQueueGeneration` restarts it (cpp:28715).
- Key plumbing: settings, parse, fingerprint, change log and dump.
- `RendererProfileConfig.h:750-751`; `ConfigEditorWindow.cpp:8280, 8290`.

## 1. R3-1: camera moves and scrolls read as cuts

**Cause.** The 0.5 s tile copies cannot tell a camera move from a cut: a dolly to an extreme close-up
decorrelates every tile (0.91-1.00, level jump 0.27-0.29); so does a credits scroll (0.93-0.99) and a
pass through the sheep's dark wool (jump 2.4 relative to a near-zero level).

**Fix.** Use the motion evidence the monitor already has.

- **The look-ahead.** It runs on every queued frame while auto is active. Its per-frame judge (the
  shape-aware rule at the per-frame motion level) sees no jump between consecutive frames in a camera
  move or a scroll. When it covered every frame of a tracker step, a tile cut also needs a frame cut
  inside the step. The tile test is then the shape alone (`coveredAlone` 0.7, or 0.4 with a 0.2 jump),
  without the 0.5 s motion margin.
- **The luma distribution.** It is position-free. A step whose distribution moved less than 0.1 is no
  cut: credits 0.03-0.06, real cuts at least 0.14.
- **Dark frames.** A frame below 1 % of the range above black is not compared, at either level. Across
  such frames only the per-frame alone path counts, at the motion level. A candidate whose flash window
  runs into darkness is cancelled and judged across it.

**Proof (real-footage suite).**

- `cosmos_longtake` (native) had cuts at 28.53, 31.03, 34.53 and 35.03 s. It now has none.
- `cosmos_longtake@upscale2x` engaged at 28.65 s "at a cut" with no fade. It now crossfades at
  30.16 s (expected 30.11), fade 1.5 s.
- The credits scroll had 6-7 cuts. It now has none.
- Native trims, recall against the library's scene list, cut fixes on vs off:

| Clip | Recall on | Recall off | Unlisted on | Unlisted off |
|---|---|---|---|---|
| Cosmos | 0.94 | 0.81-0.88 | 29-30 | 49-50 |
| Meridian | 0.89-0.95 | 0.74 | 2 | 5 |
| ToS | 0.73 | 0.67-0.80 | 7-10 | 16-28 |

  Unlisted cuts are not in the scene list: false cuts, plus some real cuts the list misses.

The ignored repro `LongTakeCameraMoveIsNotACut` is now enabled and passes. The deferred suite row is
now asserted.

## 2. Lightning and flashes

A cut candidate is confirmed only when the picture does not return to the pre-cut shape: the same
judge against the pattern before it.

- With the look-ahead: within `frameFlashSeconds` (0.25 s, as frames at the measured frame period). A
  flash also cancels the switch event it raised.
- Tiles alone: at the next analysed frame (the confirmation is deferred; the cut keeps its moment).

**Results.**

- Flashes in the long take: no cut (cut fixes off: 6 extra).
- Meridian trims (lightning in the night car scene): unlisted cuts 5 -> 2.
- Gate-verify films: Meridian 6 steps flagged with no source cut, down from 11.

## 3. Shot / reverse-shot misses

A near miss (decorrelation at least max(0.7, motion + 0.3)) is a cut when the next analysed frame
confirms the new shape persists and does not return, provided:

- the level jump is under 0.2 (a jump means a fade, flash or exposure change);
- the distribution moved at least 0.25 (the gate-verify misses moved 0.31-0.63).

**Gate-verify method.** `RealFilmCutDecisionsCutFixes` on the same 2-fps films, scored by
`tools/score_film_cuts.py` with the same scene-score truth and no exclusions. Gate-verify's own run
gave about 89 % and 83 %. Reproduced here without exclusions, with the latch-fixes judge (cut fixes
off):

| film | real cuts | recall before | recall after | no-source-cut steps before | after |
|---|---|---|---|---|---|
| Tears of Steel, last 3 min | 15 | 14 (93 %) | 14 (93 %) | 27 | 12 |
| Meridian, first 7 min | 55 | 45 (82 %) | 52 (95 %) | 11 | 6 |

Meridian's interrogation shot / reverse-shots at 48.5, 58.5, 72.0, 76.0, 80.5, 86.0 and 415.5 s are
now found. Still missed: 1.2 s (the first shot, from black), 5.0 and 9.0 s (near misses not confirmed
by the next frame). ToS: the 68.0 s cut into black.

## 4. R3-2: native trims briefly declared lower

**Cause.** Fast action read three "shots" (mostly false cuts) within about a second, on a few stray
lower readings.

**Fix.** Fewer false cuts (items 1-2), and a declaration on shots now also needs half the evidence
time (5 s at the default 10 s). The real-cut standard is kept: 3-s shots still declare at 6 s.

**Results.** Every native trim is now "declared never":

- ToS 23.976/24/25/50p/PsF/telecine, previously declared 0.75 at 51-100 s;
- Cosmos 3-min, previously 0.50-0.85 at 31-35 s and 111 s;
- tos_dark.

The suite now asserts "native never declared". The cut-fixes-off lane still shows the near misses.

## 5. The uncropped first frame (K3)

`StartupCropHold` (presentation only). From each queue generation's start (start-up, renderer reset,
live queue reset) until its first crop decision, the output is cleared to black. The detector, the
subtitle worker, A-lens, edge blanking and the crop decision itself run unchanged on the source frames.

- **Decision.** A crop applied (crop / fill / NLS) or a trusted full raster ends the hold at once. So
  does the bound (2.5 s).
- **Same source after a reset** (size and source format key): the remembered decision is shown at once.
  - A remembered crop is re-applied to the source rectangle before the screen fit.
  - A remembered full raster needs no hold.
  - Under NLS, black instead of a remembered crop: NLS maps its own geometry.
- **No hold** when automatic crop and NLS are both off.
- **Log.** Every hold transition is logged: "Active picture start-up hold: ...".

CPU tests cover the policy: black until the decision, remembered after a reset, a new source black
again, the bound, full raster, and switches off. The on-screen timing needs the GPU (below).

## Tests (x64 Release, single-threaded vstest, TRX in `D:\codex\videoprocessor\h3-latch-trx`)

- New: `SourceResolutionCutFixesTests` (8: flash cancel/cut stands, dark occluder, camera move with
  the look-ahead, tile flash and cut moment, shot/reverse-shot, unchanged distribution, dark pass,
  fast cuts never declare) and `StartupCropHoldTests` (2). `StreamStartMotionIsNotACut` was updated:
  the confirmation comes one analysed frame later, at the cut's own moment.
- Real-footage suite (`VP_TEST_CLIP_MANIFEST=D:	estclips\manifest.json`, footage required): 14/14,
  covering `RealClipSourceResolutionCutsAndLatch`, `LongTakeCameraMoveIsNotACut`,
  `RealFootageRegressionTests` and the new classes; about 24 min. The suite now models the renderer's look-ahead (3 queued frames shown
  to `LookAheadFrame` before the frame is rendered) and a cut-fixes-off lane.
- Latch, detector and mixed-screen classes, with `SourceResolutionMonitor*`: 73/73.
- Crop and layout classes (ActivePicture*, AlphaSourceCrop*, NLS, anamorphic, remembered edge, local /
  sparse / opposing boundary, captions, side picture, handoff): 907/907.
- Film cuts (`RealFilmCutDecisionsCutFixes`, `VP_TEST_CUT_FILMS`): 1/1; scores in the table above.
- Config tests (`tools/run_config_tests.ps1 -Shards 1`, off screen), including "every schema key has a
  Config control": 142/143, 1 skipped (needs a window).
  - The failure, "remote-only client opens its sole discovered VP", is a timing flake. It also fails on
    the base build (realclip 8ceaaa33) and passed on a retry here.
- Build: full x64 Release solution, rc=0.
- `FadesAndFlashesAreNotCuts` found a real case. A content jump inside an exposure ramp (decorrelation
  0.75, jump 0.14, distribution 0.19) read as a near miss. The fix: the near-miss path needs a same
  level and a 0.25 distribution move. The look-ahead's shape-and-level floor is 0.6, against real cuts
  of at least 0.72.

## GPU tests for Claude (not run here)

- `RendererCaptureHarnessTests.SourceResolutionRebasesThroughTheRenderer` and
  `RebaseKeepsTheLayoutWithCropZoomNlsAndBlanking`: the cut-moment switch and the crop under re-base,
  with `active_picture_startup_hold` on.
- K3 timing: the first present after a cold start and after a live queue reset should be black or the
  remembered crop, never the uncropped frame. Look for "Active picture start-up hold" in vp.log.
- `EveryProfileKeyChangesTheLiveSettingsFingerprint` and `IntegrationKeysReachTheSettings...`: the two
  new keys.
- A renderer replay of `cosmos_longtake@upscale2x`: a crossfade at about 30 s and no switch at 28.5 s.

## Notes

- With the look-ahead not live (an empty queue), engagement still uses the immediate tile moments.
  A camera move there can still be a switch moment, as before; the shot count no longer takes it.
- No rate or resolution is hard-coded. The flash window is seconds turned into frames at the measured
  frame period, and the thresholds are shares of the range or the picture.
- Native 4K is never re-based. `NativeFourKNeverEngages`, `UvgFourKUniformNeverEngages` and the long
  single native shots pass.
