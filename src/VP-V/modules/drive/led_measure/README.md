# LED measurement (`drive.led_measure`)

Unit: **drive** | side: output | order: 220 | status: **built** | hook point: PRE_OUTPUT

## Purpose / why

Measure the picture's light for the LED drive: the rendered picture at PRE_OUTPUT before the compensation gain (after Stage 13), and the queued raw frames for the look-ahead. Peaks are picture-only (bars and detected subtitles left out) with full coverage.

## Contract

- Input: target-light picture at output size (PRE_OUTPUT texture), plus the queued raw capture frames (luma) for the look-ahead
- Output: unchanged picture + measurement readback (`RawMeasurement` -> `FrameStats`: APL, highlight percentile and area, sustained APL/highlight, peak, cut, fade, histogram), plus predicted `FrameStats` for queued frames
- Geometry: picture area only: the trusted active picture (bars excluded) and, with `led_picture_only`, detected subtitle boxes masked
- Delay share: none added to the picture. The GPU readback is collected a few frames later (ring of 4, never waits on the GPU). The LED latency (~80 ms, `led_latency_ms`) belongs to `drive.projector_control` / `LedDriveModule`
- Look-ahead: every queued frame the renderer's walk visits (the whole queue when the LED looks ahead; `led_lookahead_ms` caps it), measured on the CPU from raw frames and corrected against the exact measurement

## How the code works

### Public API

- `MeasurementHook` (src/VideoProcessor-Lib/led/LedMeasurementHook.*, still at the legacy path). A native libplacebo hook at `PL_HOOK_PRE_OUTPUT`: one compute pass writes 3x3 block means (up to 320 x 180 blocks) into a storage buffer.
  - Per frame, before rendering: `SetFrame` (rect, APL weights, gamma-boost exponent, source sequence), `SetPeak`, `SetMask`, `SetGrain`.
  - After rendering: `Collect` returns finished measurements and their sequences.
  - `Release` before the GPU goes away. It never modifies the picture.
- `RawMeasurement` (LedMeasurement.h): the exact GPU buffer layout (256-bin histogram, 32 x 18 cell sums, total, blocks, peak).
- `StatsBuilder::Build(raw, timeMs)` (LedMeasurement.cpp): raw counts to `FrameStats`. Highlight percentile from the histogram; sustained highlight/APL as window minima (`highlight_persist_ms`, `apl_persist_ms`); cut = mean cell difference above `cut_threshold`; fade = APL falling for `fade_ms` at `fade_min_slope` or more.
- `ActivePictureRect`, `SourceRectInCrop`, `AreaPeakGrid`, `AreaAveragedMaximum` (the reference), `AreaPeakAccumulator` (the fast separable form), `HistogramPercentile`.
- `LookaheadObserver` (LedLookahead.h) implements `QueuedFrameStatistics::IObserver`: `BeginWalk`, `WantsFrame`, `ObserveFrame`. Each queued frame is measured once, in source order, with `MeasureRawFrame` (3x3 blocks on a grid of at most 96 x 54, plus `FullCoveragePeak`), then built into stats and passed to `LedDriveModule::SubmitPrediction`. `SetPictureGeometry` passes the screen size and current subtitle masks.
- The raw-to-output transfer used by `MeasureRawFrame` is built by `BuildRawTransfer` (src/VideoProcessor-Lib/led/LedRawTransfer.*, legacy path) from the renderer's colour mapping.

### Data flow

1. Render thread, per frame: the renderer sets up `MeasurementHook` and pushes it before the compensation hook (only if slot `drive.led_measure` runs).
2. After the render, `Collect` -> `LedDriveModule::SubmitMeasurement`, which runs `StatsBuilder` and updates the module's latest stats and its prediction correction ratio.
3. The queued-frame walk (also on the render thread, `AnalyzeActivePictureLookahead`) calls the observer for each new queued frame -> `SubmitPrediction`.
4. The module applies the smoothed exact/predicted ratio (`CorrectionRatio`) to predictions before the controllers use them.

### Threading

- Everything here runs on the render thread. `MeasurementHook` has no lock: the renderer calls it only from that thread.
- `LookaheadObserver` keeps its own `StatsBuilder` (owned by the walking thread) and a `geometryMutex` for `SetPictureGeometry`.
- The module calls (`SubmitMeasurement`, `SubmitPrediction`, `SetLookaheadDepth`) take the module's mutex briefly.
- Cost: the full-coverage peak on the raw path is about 2-3 ms per new 4K P010 frame on one core (see `LookaheadPeakCostAt4K`).

### Config keys actually read

- `[led_drive.default]` (alias `[dynamic_drive.default]`):
  - Read in StatsBuilder: `highlight_percentile` (99.5), `highlight_persist_ms` (250), `apl_persist_ms` (100), `cut_threshold` (24), `fade_ms` (250) / `fade_min_slope` (12; the older `fade_frames` / `fade_min_step` are converted), `script_highlight_code` (215, highlight area).
  - Passed in by the renderer: `apl_weights`, `led_peak_full_coverage` (on), `led_picture_only` (on), `led_highlight_grain_suppress` (on; needs the Stage 8 grain map), `gamma_boost` (the stats describe the boosted picture).
  - Read by the module (look-ahead depth and use): `lookahead` (on), `script_lookahead` (on), `led_lookahead_ms` (0 = whole queue).
- Renderer profile `source_range` (how the raw path reads the captured range).
- Mismatch: `led_compensation` and `led_protect` (Settings below) are not read by this code. `led_compensation` is not a config key at all. `led_protect` is a value of the renderer profile key `intent_black`, which limits the LED floor through `LedDriveModule::SetIntentLimits`, not the measurement.

### What the user sees when it falls back

- libplacebo disables the measurement pass: log "Dynamic Drive: WARNING libplacebo disabled the measurement pass; the LED keeps its last statistics". No other notice.
- Slot `drive.led_measure` off (`pipeline_slots_off`): no measurement. Compensate never steps before its first stats, and after that keeps acting on the last ones; script mode keeps ticking on the last stats. Nothing is shown.
- A readback not collected in time: the frame is skipped (`MeasurementHook::Dropped`, never reported).
- The listed "Fallbacks engaged" entry `LED: drive held (measurement unavailable)` is not implemented (no `EngageFallback` call for it). "Holdover" in its behaviour means the controllers keep the last statistics. It is not `LedDriveHoldover` (the between-files hold in `drive.led_compensation`).

### Dependencies (to lift it)

- LedController.h from `drive.projector_control` (`FrameStats`), led/LedDriveConfig.h.
- QueuedFrameStatistics.h and `AnalysisLumaSource` (renderer queue walk).
- `LedDriveModule` (`drive.led_compensation`) as the consumer: `LookaheadObserver` calls it directly.
- libplacebo (LedMeasurementHook, LedRawTransfer), both still in src/VideoProcessor-Lib/led/.

### Test it standalone

- `LedDriveMeasurementTests` (CPU: stats units, cuts and fades, peak grid, `AreaPeakAccumulator` vs the reference, subtitle masks, 4K cost).
- `LedDriveHunt3Tests` (StatsBuilder windows, look-ahead clock across rate changes).
- `LedScriptCorrelationTests`: a no-op unless `VP_TEST_LED_CORRELATION=1` and `VP_TEST_REAL_FOOTAGE` are set.
- GPU (in `LibplaceboLutParserTests`): `LedMeasurementHookMeasuresWithoutTouchingThePicture`, `LedMeasurementHookFullCoveragePeakAndPictureOnlyMask`, `LedHighlightStatisticIgnoresGrain`. They run on WARP; `VP_TEST_GPU=hardware` uses the real GPU.
- Run one class: `vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~LedDriveMeasurementTests" /Logger:"trx;LogFileName=x.trx"`. Tests run single-threaded; read the TRX.

## Settings

- Existing keys: the measurement keys of `[led_drive.default]` listed in "Config keys actually read". The listed `led_compensation` and `led_protect` are not read by this module.
- Planned keys: (none)
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entry: LED: drive held (measurement unavailable). Not implemented in code; today only a log warning when libplacebo disables the pass.
- Behaviour: The controllers keep acting on the last statistics (not the between-files `LedDriveHoldover`).

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `colour.appearance`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA` (the LED hook tests run on WARP by default; `VP_TEST_GPU=hardware` for the real GPU).

Placeholder tests:
- (none yet)

Isolation tests:
- LedMeasurement tests (`LedDriveMeasurementTests`, GPU hook tests in `LibplaceboLutParserTests`)
- LED correlation harness (`LedScriptCorrelationTests`)

Contract tests:
- measures after Stage 13, before compensation (slot order enforced by `OrderValidator` / `SlotContracts`, `PipelineSkeletonTests`)

Chain tests:
- LED + subtitles at a 2-frame queue (no dedicated test found)

## Known issues / open items

- The "Fallbacks engaged" entry is not wired; dropped readbacks are not reported.
- The exact stats are time-stamped at collection (`NowMs()` in `SubmitMeasurement`), not at the frame's display time.

## Where the code lives (live file list)

Code location: **partly migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/drive/led_measure/LedMeasurement.cpp`
- `src/VP-V/modules/drive/led_measure/LedLookahead.cpp`
- `src/VP-V/modules/drive/led_measure/LedMeasurement.h`
- `src/VP-V/modules/drive/led_measure/LedLookahead.h`
- `src/VideoProcessor-Lib/led/LedMeasurementHook.cpp`

`LedMeasurementHook.*` stays at the legacy path for now: `VideoProcessor-Test.vcxproj` (changed by active branches) compiles its `.cpp`.

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0013 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `led_compensation`, `led_protect`.
- Tiers: not tiered.
- VP-V page, Dynamic Drive tab: not tiered (Not a quality/cost choice: Dynamic Drive is set up on its own page (projector, script/compensate).).
- Plain-language guide: [docs/settings/VP-settings-reference.md#7-dynamic-drive](../../../../docs/settings/VP-settings-reference.md#7-dynamic-drive).
- Fallback entry ("Fallbacks engaged"): `LED: drive held (measurement unavailable)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): LedMeasurement tests; LED correlation harness.
- Contract (formats, geometry, fallback entry): measures after Stage 13, before compensation.
- Chain (total delay at the 4/3/2-frame budgets): LED + subtitles at a 2-frame queue.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
