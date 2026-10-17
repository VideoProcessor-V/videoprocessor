# Source-resolution latch fixes (test/srcres-latch)

Branch `test/srcres-latch` (fork), based on 9afc09da. Everything sits behind `[vprenderer]
source_resolution_latch_fixes` (default on). Off gives the earlier behaviour, and the tests check that.

## Before / after (CPU, real footage D:\hfr, 24 fps unless stated)

| Scenario | Before (off) | After (on) |
|---|---|---|
| Native 4K: clean, grain, fog, dark, UVG, long single shots | 0 engages | 0 engages, never declared |
| Evidence standard | 3 shots AND 10 s | 3 shots OR 10 s (declared at 6.0 s with 3-s shots; 10.0 s in one take) |
| Re-declare after 0.5 / 3 / 30 s of native | 120 s | 10.0 s |
| Menu over an engaged upscale, resume | 124 s | 12.1 s, at a cut |
| Native -> upscale | engaged 124 s after the first upscaled cut | 12.1 s, at a cut |
| Long dark scene (x0.12) | 1 false-native unlatch | 0 |
| Defocused native after an engaged upscale | native in 0.125 s | native in 0.125 s (kept by the contrast gate) |
| 4:3 with player-added bars | never engaged | engaged at a cut, class 0.5000 |
| In-shot fades / flashes, 7 real cuts | 23 cuts counted | 7 counted |
| Long take with no cut | never engaged | crossfade 1.5 s at declared + 20 s (30.1 s) at 24/50/60 |
| Fast pan in a long take | - | switched at once during the pan (15.0 s) |
| Ladder 1080->4K->1080 mid-shot | unlatch 0.10-0.125 s | same; re-engage at the next cut (14.1 s) at 24/50/60 |
| Clock gap > 2 s | logged "detail above the re-based height" | "a gap of 2.6 s with no frames", whole monitor reset |
| Release | hard switch | immediate, faded <= 0.25 s (checked on every run) |
| Synthetic hour, 0.2 % false-native frames | native 45 % of the hour | native 4.8 % (172 s) |

## Changes

- `src/VideoProcessor-Lib/SourceResolutionDetector.h/.cpp`: the tile shape (spread, quadrants, bar runs) in
  `Analyse`. Bar tiles at cpp:777 are left out of the curves, the mixed guard, the mean and the pattern.
  `FrameReading::black/pattern/quantisationFloor/contrast` (cpp:827-838). `PatternChange` (cpp:939) and
  `Tracker::CutJudge` (h:408) give shape-aware cuts (cpp:1093). The level-aware native test (cpp:1143)
  is gated by `nativeFloorContrast` 0.04 (h:391). A native reading clears the window (cpp:1175). The
  rendition streak is now in seconds (cpp:1201). Shots OR time (cpp:1246). `TileSize` (cpp:976).
- `src/VideoProcessor-Lib/vprenderer/SourceResolutionMonitor.h/.cpp`: the tile is a fraction of the
  height (GPU copy and look-ahead, cpp:143/445). The look-ahead uses shape cuts and a real black
  (cpp:200). `ResetEvidence` (cpp:590) runs on a gap or clock reversal with its own reason (cpp:675). An
  internal tracker reset also clears votes, history and the per-axis state (cpp:686). `EngageTrigger`
  (cpp:1098) covers cut, real black, fast pan, low detail, or a crossfade after the wait. `Transit` and
  `AutoTransition` (cpp:1126) handle the fade, with `FadeMixTimestamps` and `FadeMixValue` at cpp:1172.
- `src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp`: keys and parse (2264, 2558), the
  optical-flow global vector for the pan (13685), and the crossfade through `pl_render_image_mix` with
  the triangle mixer (20694).
- `src/VideoProcessor-Lib/RendererProfileConfig.h:725-728` validates the keys.
  `src/VideoProcessor-Config/ConfigEditorWindow.cpp:7843, 8073` adds the controls (Config test "every
  schema key has a Config control" passes).
- Tests: `SourceResolutionLatchTests.cpp` (new tests at 1864-2089; every `Run` asserts no off-cut
  engagement (874) and a release fade <= 0.25 s (872)), `SourceResolutionDetectorTests.cpp`,
  `LibplaceboLutParserTests.cpp` (old-behaviour tests pinned to fixes off).

## Switches and keys

| Key | Default | Range |
|---|---|---|
| `source_resolution_latch_fixes` | on | on / off |
| `source_resolution_engage_max_wait_s` | 20 | 0-600 |
| `source_resolution_crossfade_s` | 1.5 | 0-10 (0 = hard) |
| `source_resolution_release_fade_s` | 0.25 | 0-0.25 |

Internal settings: `nativeFloorDecades` 0.5, `nativeFloorContrast` 0.04 (measured: false natives
<= 0.010, defocused native >= 0.118), cut decorrelation 0.4 with a 0.2 jump or 0.9 alone, plus 0.3 over
the motion level (measured: in-shot <= 0.78, fades <= 0.21, cuts >= 0.55). Fast pan >= 0.5 picture
heights/s.

## Tests

One process with TRX: `SourceResolutionLatchTests`, `SourceResolutionDetectorTests`,
`MixedScreenSourceResolutionTests`, `SourceResolutionMonitorIdentifiesThePlayerKernel`, and
`SourceResolutionLookAheadVerifiesOrCancelsTheSwitch`: **69/69 passed, 19.5 min**, with no GPU. New
tests: `CrossfadeMixIsMonotonicAndExactAtTheEnds`, `LongTakeCrossfadesAfterTheWait` (24/50/60),
`FastPanSwitchesInALongTake`, `FadesAndFlashesAreNotCuts`, `DarkAndSoftNativeUnlatchQuickly`,
`Source1080UsesHeightRelativeTiles`, `LatchFixesCalibration`, `TrackerResumeAfterNativeEvidence`.
`NativeFourKNeverEngages` was extended with long single shots.

## Crossfade GPU cost (estimate; not measured on hardware)

During the fade only (1.5 s), every frame draws both paths through libplacebo's main pipeline into
rgba32f mix caches (the VP fork's `fbo_float32`). They are then blended once before the output stage.
Extra cost:

- one more full main render, about the scaler and colour pipeline's own time;
- one SourceRebase pass when the old path is re-based;
- about 0.5 GB/frame of extra 4K rgba32f traffic (two cache writes plus the blend reads).

| GPU | Bandwidth | Extra traffic | Total extra |
|---|---|---|---|
| RTX 3050 Ti laptop | 192 GB/s | ~2.8 ms | ~1x main render + ~3 ms |
| RTX 3090 | 936 GB/s | ~0.6 ms | ~1x main render + ~0.7 ms (about 3.5x faster) |

On the laptop a heavy scaler can drop frames during the fade. On the 3090 it should fit 24p and
very likely 60p. A cut, black, pan or low-detail switch costs nothing. Confirm on the 3090.

## Notes

- A low-detail trigger can be a fade's dark end (no informative tiles). The owner listed low-detail
  moments as allowed, so this is accepted.
- A still with dither now declares from cold (10 s, one shot) but never engages (no cut, so it waits
  and then would crossfade). The still tests assert no engagement within their 20 s; a longer still
  would crossfade.
- CI: the workflow commits (daffad7c, 70f28a35, bc0ed443) were cherry-picked so the branch runs `ci-windows`.

## CI

Run 39071448541 (https://github.com/vmcpherson2012-dev/videoprocessor/actions/runs/39071448541) was
triggered by the push of 752d748a. It reached the Install Qt step, then the GitHub API began returning
"HTTP 403: Sorry. Your account was suspended" for vmcpherson2012-dev. The gh token is now invalid and
the run page returns 404, so the CI result could not be observed. Local results above are the evidence.
