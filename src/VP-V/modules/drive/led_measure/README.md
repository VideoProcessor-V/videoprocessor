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

- `MeasurementHook` (LedMeasurementHook.*, moved here in WP-15). A native libplacebo hook at `PL_HOOK_PRE_OUTPUT`: one compute pass writes 3x3 block means (up to 320 x 180 blocks) into a storage buffer.
  - Per frame, before rendering: `SetFrame` (rect, APL weights, gamma-boost exponent, source sequence), `SetPeak`, `SetMask`, `SetGrain`.
  - After rendering: `Collect` returns finished measurements and their sequences; `LateNote()` describes readbacks running late or frames not measured (the readback ring, ReadbackRing.h), `Dropped()` counts skipped readbacks.
  - `Release` before the GPU goes away. It never modifies the picture.
- `RawMeasurement` (LedMeasurement.h): the exact GPU buffer layout (256-bin histogram, 32 x 18 cell sums, total, blocks, peak).
- `StatsBuilder::Build(raw, timeMs)` (LedMeasurement.cpp): raw counts to `FrameStats`. Highlight percentile from the histogram; sustained highlight/APL as window minima (`highlight_persist_ms`, `apl_persist_ms`); cut = mean cell difference above `cut_threshold`; fade = APL falling for `fade_ms` at `fade_min_slope` or more.
- `FrameStats` (FrameStats.h, split out of LedController.h in the WP-15 re-cut): the per-frame statistics this module produces and the controllers in `drive.led_compensation` consume.
- `ActivePictureRect`, `SourceRectInCrop`, `AreaPeakGrid`, `AreaAveragedMaximum` (the reference), `AreaPeakAccumulator` (the fast separable form), `HistogramPercentile`.
- `LookaheadObserver` (LedLookahead.h) implements `QueuedFrameStatistics::IObserver`: `BeginWalk`, `WantsFrame`, `ObserveFrame`. Each queued frame is measured once, in source order, with `MeasureRawFrame` (3x3 blocks on a grid of at most 96 x 54, plus `FullCoveragePeak`), then built into stats and handed to its `IPredictionSink` (`SubmitPrediction`). `IPredictionSink` (LedLookahead.h) is what the look-ahead needs from the drive (config, raw transfer, depth, epoch, range, period); `LedDriveModule` implements it, so this module no longer includes LedDriveModule.h. `SetPictureGeometry` passes the screen size and current subtitle masks.
- The raw-to-output transfer used by `MeasureRawFrame` is built by `BuildRawTransfer` (`drive.led_compensation`, LedRawTransfer.*) from the renderer's colour mapping; `DynamicDriveFrame` hands it to the module when the mapping changes.

### Data flow

1. Render thread, per frame: `DynamicDriveFrame::PrepareFrame` (the WP-15 host adapter, `drive.led_compensation`; one call from the renderer once the call-site patch is applied) sets up `MeasurementHook` (active picture, APL weights, boost, peak boxes, picture-only masks, grain map) and pushes it before the compensation hook, only if slot `drive.led_measure` runs.
2. After the render, `DynamicDriveFrame::CollectMeasurements`: `Collect` -> `LedDriveModule::SubmitMeasurement`, which runs `StatsBuilder` and updates the module's latest stats and its prediction correction ratio; then `LateNote()` for the fallback below.
3. The queued-frame walk (also on the render thread, `AnalyzeActivePictureLookahead`) calls the observer for each new queued frame -> `SubmitPrediction`.
4. The module applies the smoothed exact/predicted ratio (`CorrectionRatio`) to predictions before the controllers use them.

### Threading

- Everything here runs on the render thread. `MeasurementHook` has no lock: only `DynamicDriveFrame` calls it, from that thread.
- `LookaheadObserver` keeps its own `StatsBuilder` (owned by the walking thread) and a `geometryMutex` for `SetPictureGeometry`.
- The module calls (`SubmitMeasurement`, `SubmitPrediction`, `SetLookaheadDepth`) take the module's mutex briefly.
- Cost: the full-coverage peak on the raw path is about 2-3 ms per new 4K P010 frame on one core (see `LookaheadPeakCostAt4K`).

### Config keys actually read

- `[led_drive.default]` (alias `[dynamic_drive.default]`):
  - Read in StatsBuilder: `highlight_percentile` (99.5), `highlight_persist_ms` (250), `apl_persist_ms` (100), `cut_threshold` (24), `fade_ms` (250) / `fade_min_slope` (12; the older `fade_frames` / `fade_min_step` are converted), `script_highlight_code` (215, highlight area).
  - Passed in through `DynamicDriveFrame` (from the module's config): `apl_weights`, `led_peak_full_coverage` (on), `led_picture_only` (on), `led_highlight_grain_suppress` (on; needs the Stage 8 grain map), `gamma_boost` (the stats describe the boosted picture).
  - Read by the module (look-ahead depth and use): `lookahead` (on), `script_lookahead` (on), `led_lookahead_ms` (0 = whole queue).
- Renderer profile `source_range` (how the raw path reads the captured range).
- slots.json / MANIFEST now list exactly these keys (WP-15): `apl_persist_ms`, `apl_weights`, `cut_threshold`, `fade_min_slope`, `fade_ms`, `highlight_percentile`, `highlight_persist_ms`, `led_highlight_grain_suppress`, `led_lookahead_ms`, `led_peak_full_coverage`, `led_picture_only`, `lookahead`, `script_highlight_code`, `script_lookahead`. The old `led_compensation` (a shader rule name) and `led_protect` (a value of `intent_black`, read by the module's intent limits) are gone. `gamma_boost` is listed by `drive.led_compensation`.

### What the user sees when it falls back

- libplacebo disables the measurement pass: "Fallbacks engaged: LED measurement (libplacebo disabled the measurement pass; the LED follows the last statistics)" plus the log warning (DynamicDriveFrame::OnHookErrors; once the WP-15 call-site patch is applied). Reset when the renderer's GPU state is rebuilt.
- Slot `drive.led_measure` off (`pipeline_slots_off`): no measurement. Compensate never steps before its first stats, and after that keeps acting on the last ones; script mode keeps ticking on the last stats. Nothing is shown.
- Readbacks running late, or frames not measured (`MeasurementHook::LateNote`): "Fallbacks engaged: LED measurement (readbacks late: ... / readbacks behind: frames not measured; ...)", held until no note for 2 s (DynamicDriveFrame::CollectMeasurements). This replaces WP-4's inline LED hunk (see docs/design/wp15-led-callsites.md).
- Slot `drive.led_measure` off (`pipeline_slots_off`): a deliberate choice, no entry; compensate keeps acting on the last statistics and script mode keeps ticking on them. (Open item: should compensate hold full drive here too? Owner question.)

### Dependencies (to lift it)

- FrameStats.h (here), VP-V/modules/drive/led_compensation/LedDriveConfig.h (the drive's config schema).
- QueuedFrameStatistics.h and `AnalysisLumaSource` (renderer queue walk).
- The consumer only through `IPredictionSink` (implemented by `LedDriveModule`): no include of `drive.led_compensation` code besides the config schema.
- libplacebo (LedMeasurementHook; compiled by VideoProcessor-VPRenderer, -VPRendererBeta and -Test).

### Test it standalone

- `LedDriveMeasurementTests` (CPU: stats units, cuts and fades, peak grid, `AreaPeakAccumulator` vs the reference, subtitle masks, 4K cost).
- `LedDriveHunt3Tests` (StatsBuilder windows, look-ahead clock across rate changes).
- `LedScriptCorrelationTests`: a no-op unless `VP_TEST_LED_CORRELATION=1` and `VP_TEST_REAL_FOOTAGE` are set.
- `DynamicDriveAdapterTests.PictureOnlyMasksCoverTheSubtitleAndFollowNls` (CPU: the picture-only masks, with and without a known NLS map).
- GPU (in `LibplaceboLutParserTests`): `LedMeasurementHookMeasuresWithoutTouchingThePicture`, `LedMeasurementHookFullCoveragePeakAndPictureOnlyMask`, `LedHighlightStatisticIgnoresGrain`. They run on WARP; `VP_TEST_GPU=hardware` uses the real GPU.
- Run one class: `vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~LedDriveMeasurementTests" /Logger:"trx;LogFileName=x.trx"`. Tests run single-threaded; read the TRX.

## Cost

Efficiency review 2026-10-24 (code reading only, nothing measured unless a test is named). Ranked by estimated saving.

1. **Readback stalls the render thread every frame (D3D11).** libplacebo's D3D11 backend has no `buf_poll` (`pl_buf_poll` always says ready) and `pl_buf_read` is CopyResource + a blocking `Map` (vendored 7.360.1 source, d3d11/gpu_buf.c). `Collect` runs right after `pl_render_image`, so it reads the slot submitted this frame and waits until the GPU has finished the whole frame. The ring of 4 never fills, `Dropped` never counts, and the stats are not "a few frames old". Saving: up to one whole GPU frame time of render-thread wait per output frame (several ms at a heavy profile). Estimate from the call order and the libplacebo code; time it with a QPC around `Collect`. Fix: copy to a staging buffer VP owns right after the pass, with an event query, and `Map` with DO_NOT_WAIT a few frames later (native D3D11 interop). A lighter fix: collect before the next frame is submitted. Byte-identical: picture yes in log_only/script; in compensate no, because the stats reach the controller 1-3 frames later (the documented design). Risk medium. Check the Beta 377 DLL too. `SourceResolutionMonitor` and `AdaptiveDownscale` use the same poll/read pattern.
2. **Full-coverage peak computed where nothing reads it.** `FrameStats::maximum` is read only by `ScriptController::Tick` and `PlanScriptLookahead` (script and log_only). In compensate, the look-ahead's `FullCoveragePeak` still costs 2-3 ms CPU per new 4K source frame (`LookaheadPeakCostAt4K` target), which is 48-72 ms/s at 24p and 120-180 ms/s at 60p. Compensate and picture_only also run the GPU `PEAK_BODY` pass every output frame: every pixel, 6 mask tests, 3 `pow`. At 2560x1600 that is about 4.1 M px x ~1.6 reads x 8 B (FP16 assumed), about 0.05-0.1 ms on the 3090 and 0.2-0.35 ms on the 3050 Ti; double that at a 4K output. Fix: treat `peakFullCoverage` as on only in modes that read `maximum`. Byte-identical: yes (the field is unread there). Risk low.
3. **Look-ahead runs serially on the render loop.** Even where it is needed (script look-ahead), `MeasureRawFrame` costs 2-3 ms per new 4K frame before the render (`AnalyzeActivePictureLookahead`). Fix: run the observer on a worker thread (frames are AddRef'd; the observer owns its builder; `SubmitPrediction` is locked), or split `FullCoveragePeak` into box-row bands on 2-4 threads and take the maximum. Byte-identical: yes for values (bands must start on box-row boundaries so the row batches match); predictions may arrive later. Risk medium.
4. **Block grid reads through `Sample()`.** 96 x 54 blocks x 9 = 46,656 calls per new frame, each with a bounds check, a format switch and a chroma decode. At an estimated 5-10 ns each that is 0.25-0.5 ms per frame; the test logs it as "sparse grid alone". Fix: `LumaPlaneRow` / `SampleLumaRow` for the 3 rows of each block row. Byte-identical: yes (the row reader returns `Sample()`'s codes, and `FullCoveragePeak` already relies on that). Risk low.
5. **Grain path recomputes block means 9 times.** With `led_highlight_grain_suppress` (default on), each grainy block re-reads its 9 neighbours' 3x3 (81 fetches + `pow`). Up to 57,600 x 81 = 4.7 M fetches, about 0.03 ms on the 3090 and 0.1-0.15 ms on the 3050 Ti. Fix: block means once (shared-memory tile with a halo, or a first pass into a 320x180 buffer). Byte-identical: yes if the stored mean uses the same expression (check with the GPU hook tests: compilers may contract differently). Risk low-medium.
6. **Global atomics on single addresses.** Each block does 5-6 atomics, so `total_sum` and `total_blocks` take 57,600 each. If the driver does not aggregate them, that is about 0-0.05 ms (estimated at about one same-address atomic per L2 clock). Fix: workgroup reduction (shared memory), then one atomic per group (225 groups). Byte-identical: yes (integer sums, no overflow). Risk low.
7. **Rebuilt per frame.** `AreaPeakAccumulator` (about 200 KB of vectors at 3840 wide, plus per-column and per-row set-up) and three 1024-entry tables (`PeakValueTable`, clamped, plane). About 20-60 µs per new frame (allocation and loop sizes). Fix: cache them by geometry, transfer and range. Byte-identical: yes. Risk low.
8. **Slot `drive.led_measure` off still walks.** `WantsPredictions` ignores the slot, so the look-ahead keeps measuring (2-3 ms per new 4K frame). Without exact stats the controllers never step, so the predictions are never used. Fix: no predictions while the slot is off. Byte-identical: yes. Risk low.
9. **Per-frame mask set-up.** A `std::vector` of boxes, plus with NLS a 40-step bisection per edge (up to 960 `MapCoordinate` calls), about 10-50 µs per frame (estimated). Fix: cache by box and NLS map. Byte-identical: yes. Risk low.

When off: LED disabled costs nothing (no hook, no observer, no walk depth; a few atomic loads). log_only, script and picture_only still run both GPU passes and the readback every output frame. log_only and picture_only take no look-ahead.

## Settings

- Existing keys: the measurement keys of `[led_drive.default]` listed in "Config keys actually read".
- Planned keys: (none)
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entry: LED measurement (<reason>; the LED follows the last statistics). Engaged by `DynamicDriveFrame` (pass disabled, readbacks late / frames not measured) once the WP-15 call-site patch is applied.
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
- `src/VP-V/modules/drive/led_measure/ReadbackRing.h`
- `src/VP-V/modules/drive/led_measure/LedMeasurementHook.cpp`
- `src/VP-V/modules/drive/led_measure/LedMeasurementHook.h`
- `src/VP-V/modules/drive/led_measure/FrameStats.h`

Forwarding header at the legacy path (removed in M2 step 0): `src/VideoProcessor-Lib/led/LedMeasurementHook.h` (LibplaceboVideoRenderer.cpp still includes it until the WP-15 call-site patch lands).

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0013 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `apl_persist_ms`, `apl_weights`, `cut_threshold`, `fade_min_slope`, `fade_ms`, `highlight_percentile`, `highlight_persist_ms`, `led_highlight_grain_suppress`, `led_lookahead_ms`, `led_peak_full_coverage`, `led_picture_only`, `lookahead`, `script_highlight_code`, `script_lookahead`.
- Tiers: not tiered.
- VP-V page, Dynamic Drive tab: not tiered (Not a quality/cost choice: Dynamic Drive is set up on its own page (projector, script/compensate).).
- Plain-language guide: [docs/settings/VP-settings-reference.md#7-dynamic-drive](../../../../../docs/settings/VP-settings-reference.md#7-dynamic-drive).
- Fallback entry ("Fallbacks engaged"): `LED measurement (<reason>; the LED follows the last statistics)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): LedMeasurement tests; LED correlation harness.
- Contract (formats, geometry, fallback entry): measures after Stage 13, before compensation; picture-only masks (Dynamic Drive adapter tests).
- Chain (total delay at the 4/3/2-frame budgets): LED + subtitles at a 2-frame queue.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
