# Source analysis (`input.source_analysis`)

Unit: **input** | side: input | order: 40 | status: **partial** | hook point: host

## Purpose / why

Analyse the ORIGINAL delivered frame before anything changes it: source resolution + scaler kernel pre-analysis (per axis), active picture/crop, cut/fade/black, subtitle detection, and the labels net A needs (bit depth, ratio, kernel, bitrate tier, codec family).

## Contract

- Input: the delivered frame (untouched) as an `AnalysisLumaSource` view at delivered size. `Sample()` returns limited-range 10-bit codes (luma 64..940, chroma 512 +/- 448) for every format (P010, P210, native RGB, native 4:2:2, DV 4:2:2, SDR tunnel RGB, packed DV). Subtitle detection also reads chroma to colour-match ink.
- Output (built): subtitle boxes (`SubtitleBoxResult`, half-open source-raster rectangles), active area, scene events, effective source resolution / bitrate quality / bit depth readings.
- Output (planned, not built): per-axis kernel class + confidence as a label, A labels (label schema v1) and the re-based / native / un-reversed state flag. No code produces these yet.
- Geometry: delivered size; reports active area and subtitle boxes in delivered (source-raster) coordinates.
- Delay share: 0 frames (past-only detectors); latch confirmation is time-based.
- Look-ahead: uses queued frames where available (subtitles, active picture), never waits. The subtitle window is the current frame plus up to 7 queued frames (`SubtitleBoxLookahead::MaxFrames` = 8).
- Subtitle detection needs a letterbox bar: `SubtitleBoxDetector::Analyze` returns an empty result when the picture fills the raster (`pictureTop == 0` and `pictureBottom == height`). Boxes are diagnostic evidence only: no OCR, no pixel changes, no crop authority.

## How the code works

### Subtitle detection (migrated, this folder)

Files and public API:

- `SubtitleBoxDetector.h/.cpp`: `SubtitleBoxDetector::Analyze(source, pictureTop, pictureBottom, sequence, viewportGeneration)` returns a `SubtitleBoxResult` (cue bounds, up to 3 line boxes, source/capture card panels, bar anchor, ink signatures, `cue` id, `detected` / `held` / `revised` / `workLimit`, `diagnosticReason`). `SetScriptHint`, `SetNearBarDistance` and `SetOptimizationMode` reset the detector when the value changes. `InkSnapshot()` exposes the sampled raw ink for later cue revalidation. `SamplingStep()` samples about 960x540 (step 2 at 1080p, 4 at UHD). A lost cue is reported `held` for at most 2 frames while its bar anchor is still present.
- `SubtitleBarEvidence.h`: `ExtractSubtitleBarEvidence` (proves top/bottom bar edges from current pixels), `RevalidateSubtitleBarEvidence` (re-proves a previous bar plane), `ResolveCurrentSubtitleAnalysisBars`, `ProveSubtitleCaptionStrip`. Subtitle-only evidence, never crop authority.
- `SubtitleOpaqueCardRefinement.h`: `RefineSubtitleOpaqueCard` measures an opaque caption card around a confirmed text seed (bounded sample budget). Called from the detector.
- `SubtitleBoxLookahead.h`: `SubtitleBoxLookahead::Measure` (one frame: bars, picture bounds, detector scan, giving a `SubtitleBoxObservation`), `Resolve` (current + queued observations into a `SubtitleBoxPreview`), `CanReuseMeasurement`, `AdvanceBarTrackingReference`. Class `SubtitleBoxPresentation`: `Consume` turns previews into the presented `SubtitleBoxResult` with a hold; also `RefreshReferencePixels`, `ReusePreviewForRepeatedSourceFrame`. Class `SubtitleStabilityTelemetry`: loss / flash counters.
- `SubtitleTemporalFill.h`: `SubtitleTemporalFill::Capture` / `Matches` take a 32x18 sample signature of a clean frame and check that the area around a cleanup box still matches, so the renderer can reuse that frame as the fill donor.

Data flow:

1. The renderer prepares an `AnalysisLumaSource` from the untouched frame (`PrepareAnalysisFrame`, AnalysisFrame.h).
2. `QueueSubtitleMeasurement` (LibplaceboVideoRenderer.cpp) builds a key (frame identity, policy/continuity generations, near-bar distance, optimization mode, script hint, shared picture snapshot) and submits it to `SubtitleMeasurementWorker`. A matching ready result is taken; otherwise the call returns at once with `analyzed = false`.
3. On the worker, `SubtitleMeasurementSampler::Measure` runs `SubtitleBoxLookahead::Measure`. Picture bounds come from the shared Classic picture snapshot when it is available, else from this frame's own bar proof. Then `SubtitleBoxDetector::Analyze` runs. Accepted independent detections also feed `EvaluateSubtitleAssistedVerticalBarProof` (ActivePictureEvidence.cpp).
4. The frame-queue pass resolves the current + queued observations into the front frame's `SubtitleBoxPreview`. The render thread re-checks it against the current identity and generations. While the worker is late it tries `SubtitlePendingMeasurementGuard`, then calls `SubtitleBoxPresentation::Consume`.
5. Placement and rendering (`SubtitleCutPaste*`, layout anchor, padding, temporal/stretch/mirror fill) stay in src/VideoProcessor-Lib and consume the result.

Threading:

- `SubtitleBoxDetector` and `SubtitleBoxPresentation` are not thread-safe; each owner keeps its own instance.
- Full scans run on one CPU worker thread (`SubtitleMeasurementWorker`, SubtitleMeasurementWorker.h), started on the first submit. Up to 8 pending jobs; the oldest is dropped beyond that. Duplicate keys are coalesced. A scan that throws is counted as failed and returns `analyzed = false`.
- The render thread never waits for a scan. It runs `Resolve`, `Consume`, `RefreshReferencePixels` and `SubtitleTemporalFill` itself (cheap, bounded sampling).
- `ActivePictureEvidence.cpp` and the renderer's caption-strip check create short-lived local detectors on their own thread.

Config keys actually read (all in `[vprenderer.subtitles.<Name>]`, the profile chosen by the `subtitles` selection; subtitle keys in other sections are discarded after loading):

- `type` (`off` / `classic` / `generated_gray` / `black`): presentation style; `classic` (or empty) turns on Classic subtitle fit.
- `subtitle_script_hint` (`general` = `latin`, `cjk`, `arabic`, `thai`): `SetScriptHint`.
- `subtitle_near_bar_px` (0..200, default 20 source pixels): `SetNearBarDistance`, a second detector pass for cues near but not touching the bar.
- `subtitle_hold_ms` (0..1000, default 250): the `SubtitleBoxPresentation::Consume` hold and the pending-guard bridge.
- `cleanup_fill` (`mirror` / `stretch` / `temporal`) or legacy `temporal_fill`: picks the fill; `temporal` uses `SubtitleTemporalFill`.
- Placement-only keys in the same section (`offset_pixels`, `minimum_headroom_pixels`, `subtitle_hold_seconds`, `subtitle_engage_drift_ms`, `subtitle_release_drift_ms`, `subtitle_target_buffer_pixels`, colours, opacity, blur) do not affect detection.
- `subtitle_detection_optimization` (`baseline` / `buffers` / `shared_samples`, for `SetOptimizationMode`) is parsed only from legacy sections and then reset to `baseline`. The `subtitles` group does not accept it, so detection always runs `baseline` today.

User-visible fallbacks / notices: none. Subtitle detection registers no "Fallbacks engaged" entry. A late worker result is bridged by `SubtitlePendingMeasurementGuard`; when that fails the cue is dropped and `SUBTITLE PENDING` is logged (first 4 only). Work-limit hits (`calibration-budget`, `component-budget`) and worker drops/failures appear only in the log and the worker diagnostics.

Dependencies to lift it:

- `AnalysisLumaSource.h/.cpp` (with ColorSpace.h, VideoFrameEncoding.h) for every file.
- `SubtitleBoxLookahead.h` also needs `ActivePictureEvidence.h` and `ActivePictureDecisionTimeline.h` (`ActivePictureFrameIdentity`, `ActivePictureBounds`, `SubtitleAssistedVerticalBarProof`, `ActivePictureArDarknessEvidence`).
- `SubtitleBoxDetector.cpp` uses the project `pch.h`.
- The worker (`SubtitleMeasurementWorker.h`) and the renderer glue stay in src/VideoProcessor-Lib.

### Legacy remainder (still at legacy paths)

- `SourceResolutionDetector` (Stage 12 step B): effective source height from the luma spectrum; a reading only.
- `vprenderer/SourceResolutionMonitor`: render-thread compute pass a few times a second; effective resolution, bitrate quality and bit depth, and `source_resolution: auto` acting on them.
- `KernelInverse` (src/VP-V/modules/scaling/rebase): kernel-matched re-base inverse, used with `rebase_kernel_match`.
- `ActivePictureAnalyzer`: active picture / crop decisions.
- `SceneDetector`: sparse P010 luma cut/fade/black detector.
- `AnalysisFrame`: canonical analysis-frame selection and preparation, shared by live and queued analysis.
- `source_resolution`, `rebase_kernel_match` and `source_coding_fixes` are read from the renderer profile section (validated in the `input` group, RendererProfileConfig.h).

### Test it standalone

Build x64 Release, then run single-threaded from the repo root, one class at a time:

```
vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~SubtitleBoxDetectorTests" /Logger:"trx;LogFileName=x.trx"
```

Read the TRX for the result. Classes:

- `SubtitleBoxDetectorTests`: the detector (scripts, cards, near-bar, work limits).
- `SubtitleBoxLookaheadTests`: Measure / Resolve / Presentation / bar tracking.
- `SubtitleOpaqueCardRefinementTests`: card refinement.
- `SubtitleMeasurementWorkerTests`: worker keys, coalescing, pending guard.
- `ActivePictureEvidenceTests`, `AnalysisRecoveryParityTests`: subtitle-assisted bar proof and analysis parity.
- Legacy part: `SourceResolutionDetectorTests`, `SourceResolutionLatchTests`, `SceneDetectorTests`.

`SubtitleTemporalFill` has no unit test.

Probes (offline, no decoder): tools/subtitle_bbox_probe (stdin P010 frames, args `width height pictureTop pictureBottom [script]`, CSV out), tools/subtitle_clip_probe, tools/subtitle_opaque_card_probe, tools/subtitle_worker_probe. The Python drivers tools/test_subtitle_*.py need numpy/opencv and a built probe. Exact native frame capture: `VP_SUBTITLE_CAPTURE_PATH` (see AGENTS.md).

## Settings

- Slot keys (slots.json): `source_resolution`, `rebase_kernel_match`, `subtitle_fit`, `subtitle_reposition`, `subtitle_type`, `source_coding_fixes`
- As read today: `source_resolution`, `rebase_kernel_match` and `source_coding_fixes` are live. `subtitle_type` is the key `type` in `[vprenderer.subtitles.<Name>]`. `subtitle_fit` outside that section is discarded (fit follows `type`). `subtitle_reposition` (main config / command line) is parsed and ignored; repositioning is permanently disabled. Detection keys: see "How the code works".
- Planned keys: `label_detector`
- Tiers: not tiered; label-detector net optional

## Fallbacks

- "Fallbacks engaged" entry: Source analysis: labels unknown (<reason>). Planned, NOT implemented: no labels are produced and nothing engages it.
- Behaviour (planned): Low confidence -> 'unknown' labels (A is trained with ~12% unknown); un-identified kernel -> Lanczos re-base or no re-base.
- Today: subtitle detection has no fallback entry (see "User-visible fallbacks / notices").

## Net fingerprint / upstream

- Fingerprint: label schema v1 (shared with repair.encoding)
- Upstream modules: `input.capture`, `input.signal`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`. Standalone commands: see "Test it standalone" above.

Placeholder tests:
- (none yet)

Isolation tests:
- SourceResolutionDetector tests
- SceneDetector tests
- subtitle corpus probes
- SubtitleBoxDetectorTests, SubtitleBoxLookaheadTests, SubtitleOpaqueCardRefinementTests, SubtitleMeasurementWorkerTests

Contract tests:
- analysis reads the original frame even when re-base/repair are on

Chain tests:
- srcres latch + crossfade checks

## Known issues / open items

- blind kernel estimation for the re-base (item 18)
- label-detector / identification net (round 2; owner idea: identification net feeding the kernel-matched re-base)
- estimator gap analysis vs ground truth (item 8)

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VideoProcessor-Lib/SourceResolutionDetector.cpp`
- `src/VideoProcessor-Lib/SourceResolutionDetector.h`
- `src/VideoProcessor-Lib/vprenderer/SourceResolutionMonitor.cpp`
- `src/VideoProcessor-Lib/vprenderer/SourceResolutionMonitor.h`
- `src/VP-V/modules/scaling/rebase/KernelInverse.cpp`
- `src/VideoProcessor-Lib/ActivePictureAnalyzer.cpp`
- `src/VideoProcessor-Lib/SceneDetector.cpp`
- `src/VideoProcessor-Lib/SubtitleBoxDetector.cpp`
- `src/VideoProcessor-Lib/AnalysisFrame.cpp`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0004 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `source_resolution`, `rebase_kernel_match`, `subtitle_fit`, `subtitle_reposition`, `subtitle_type`, `source_coding_fixes`.
- Planned keys (not yet implemented): `label_detector`.
- Tiers: not tiered; label-detector net optional.
- VP-V page, Input tab: not tiered (Always on: capture and timing have no quality steps, only paths (the toggles below).).
- Plain-language guide: [docs/settings/VP-settings-reference.md#2-input](../../../../docs/settings/VP-settings-reference.md#2-input).
- Fallback entry ("Fallbacks engaged"): `Source analysis: labels unknown (<reason>)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): MISSING.
- Isolation (fixed inputs, expected outputs, cost): SourceResolutionDetector tests; SceneDetector tests; subtitle corpus probes.
- Contract (formats, geometry, fallback entry): analysis reads the original frame even when re-base/repair are on.
- Chain (total delay at the 4/3/2-frame budgets): srcres latch + crossfade checks.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
