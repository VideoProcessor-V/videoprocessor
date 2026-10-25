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
- `VP-V/modules/input/source_analysis/SourceResolutionMonitor`: render-thread compute pass a few times a second; effective resolution, bitrate quality and bit depth, and `source_resolution: auto` acting on them.
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

## Cost

This slot's code is **Bill's** (upstream `v1.4.02-beta`). It stays at its path and VP does not change it
(owner principle 2026-10-24). The notes below describe how VP drives it and what that costs. Changes
inside Bill's files are suggestions for Bill, or for a VP-V adapter around them (M2 plan, option (d)).

Review 2026-10-24, from reading the code only. Nothing was built or measured. Every number is an estimate from operation counts. Measure the real cost with the worker diagnostics (`fullMs`, `fullPeakMs`) or the `analysisMs` trace metric.

Size of one scan: the detector always samples a fixed grid of about 960x540 (step 2 at 1080p, 4 at UHD), so the sample counts are the same at 1080p and 4K. Only the memory traffic differs: 4K touches about twice the bytes. Each `Detect` pass reads the bar rows plus `max(24, h/7)` grid rows on the picture side of each bar. For 2.39:1 that is 292 of 540 rows, about 280k samples (300k on the near-bar pass). The pass then makes about 5 more sweeps over those rows (histogram/calibration, ink mask, black plane, ink bits, labelling) and 2 whole-grid memsets (`m_mask` 518 KB, `m_labels` 1 MB). Estimate: about 2-3 ms per pass on one core for P010. On a frame with no cue (most frames), `Analyze` runs 2 passes, because the `subtitle_near_bar_px` retry runs whenever the first pass finds nothing. With `ExtractSubtitleBarEvidence` (about 40k samples) that is about 4.5-7 ms of worker CPU per source frame. This runs on the worker thread, not the render thread, unless noted.

Ranked by saving:

1. **The near-bar retry reads every pixel again.** It refetches all samples although the pass-1 rows are identical (same `sy` mapping; only the depth grows).
   - Saving, per no-cue frame: about 0.8-1.2 ms (1080p) or 1-1.5 ms (4K) for P010/P210/decoded DV. About 2-4 ms for formats read through `Sample()` (native RGB, V210/UYVY, packed DV, SDR tunnel, full-range planes).
   - How estimated: 280k fetches x 8-12 instructions, plus 2.2 MB (1080p) or 4.5 MB (4K) of rows from DRAM.
   - Fix: always use the existing `shared_samples` reuse (`m_sampledRows`/`m_rawLuma`). It is coded but the config forces `baseline`. It adds about 0.1 ms of stores to pass 1.
   - Byte-identical: yes. `SubtitleBoxDetectorTests` already checks baseline against shared_samples (P010 and RGB, near-bar 20) for results, cue ids and ink snapshots.
   - Risk: low.
2. **`AnalysisLumaSource::Sample()` re-runs `IsValid()` on every call.** `CheckedMultiply` makes 1-2 64-bit divisions per call.
   - Saving, non-planar formats: about 1-3 ms per pass (2 passes per no-cue frame unless item 1 is done).
   - Saving, planar formats: about 0.1-0.5 ms. This covers bar evidence (about 40k calls), card refinement, and on the render thread `RefreshReferencePixels` (up to 65k) and `SubtitlePendingMeasurementGuard` (up to 32k).
   - How estimated: 280k calls x 10-40 cycles of division and branch work.
   - Fix: validate once per `Analyze`/`Measure` and sample through an unchecked accessor that keeps the x/y bounds check. Or replace the division-based overflow test with a multiply-overflow intrinsic, which helps every module.
   - Byte-identical: yes. Same decode, same coordinates, and the source does not change during a call.
   - Risk: low.
3. **`SubtitleBoxLookahead::Measure` scans the same planes twice.** On the shared-picture path, when the bars were revalidated at the reference, it runs a probe `Analyze`. Then it calls `Reset()` and runs `Analyze` again at the same planes whenever the re-resolution leaves `bars.top`/`bars.bottom` unchanged.
   - Saving: one whole `Analyze` (2-6 ms) on each frame that takes this path. How often is not known; check with the worker trace.
   - How estimated: from the loop bounds of 1-2 passes.
   - Fix: when the probe planes equal the final planes, reuse `candidate` and `candidateInk`, or skip the `Reset()` so the detector's same-sequence cache answers.
   - Byte-identical: boxes, signatures and ink, yes. The detector-internal `cue` serial is one lower. `SubtitleBoxPresentation` assigns its own cue, but check that no consumer compares raw observation cues.
   - Risk: low.
   - The tracked-bottom and caption-strip probes use different planes, so they cannot be reused this way.
4. **The render thread builds fresh detectors.** The caption bootstrap cold probe (about once a second, plus bridge frames) and `EvaluateCaptionBackedVerticalBarHypothesis` each construct a new `SubtitleBoxDetector`. Its first `Detect` allocates and zero-fills about 4.7 MB (`m_luma` 1 MB, `m_chroma` 2 MB, `m_mask` 0.5 MB, `m_labels` 1 MB).
   - Saving: about 0.5-1.5 ms per probe, as a spike rather than an average. The probe's own 1-2 passes (2-6 ms) also run on the render thread.
   - How estimated: about 1150 first-touch page faults plus a 4.7 MB memset.
   - Fix: keep one member detector and `Reset()` it before each probe.
   - Byte-identical: yes, as long as `Detect` never reads scratch outside the rows it wrote. The worker's reused scanner already relies on this; confirm with the tests.
   - Risk: low.
5. **Allocations in every `Detect`.**
   - On every pass:
     - a new ink snapshot (`make_shared` plus 3 zeroed bit planes of 65 KB each); pass 1's snapshot is thrown away when the retry runs
     - `panelLeft`/`panelRight`, the `panel*Pixels` rows and a `glyphEnvelope` per candidate
   - On the detected path, two more zeroed full-grid buffers: `cardInk(area)` and `recoveredOwners(area)`, 518 KB each.
   - Saving: about 0.05-0.2 ms per pass.
   - How estimated: 0.2-1.2 MB of allocation plus memset.
   - Fix:
     - Keep member scratch buffers; `assign` keeps capacity.
     - Recycle a snapshot when its `use_count()==1`.
   - Byte-identical: yes.
   - Risk: low.
6. **The full-grid memsets run on every pass.** `m_mask` and `m_labels` are cleared over the whole grid, although only about 55% of the rows are used.
   - Saving: about 0.05-0.1 ms per pass.
   - How estimated: 1.5 MB at about 15-30 GB/s.
   - Fix: clear only the rows that were written. The panel and weak searches also write outside the default rows, so this needs dirty-row tracking.
   - Byte-identical: yes, if the tracking is complete.
   - Risk: medium. Low value.
7. **The worker repeats the active-picture extraction for live submissions.** The two live `QueueSubtitleMeasurement` call sites pass no `nativeEvidence`, so on accepted detections the worker re-runs `ExtractActivePictureEvidence` (a budget of under 30k reads plus the strict profiles). The frame-queue path already passes cached evidence.
   - Saving: about 0.2-0.5 ms per such job. These jobs are rare: live submits happen only when the queue result is missing.
   - Fix: pass the frame's cached evidence when it comes from the same prepared domain.
   - Byte-identical: yes when it comes from the same prepared source, otherwise no.
   - Risk: low.
8. **Bar finding overlaps with active-picture analysis.** `ExtractSubtitleBarEvidence` (about 40k samples, walked column by column) and `ExtractActivePictureEvidence` each find the letterbox planes of the same frame. The render thread calls `ExtractSubtitleBarEvidence` again in the pending guard and in `RecoverSubtitleInspectionBars`.
   - Saving: about 0.3-0.8 ms.
   - Fix: merging the two is a design change, because subtitle bar evidence is never crop authority.
   - Byte-identical: no.
   - Not recommended now.

Checked and found not worth changing:
- **No luma copy into the worker.** Jobs hold the prepared frame through `shared_ptr` (`PrepareFrameAnalysis` caches it per queued frame).
- **No GPU readback.** The path reads the captured CPU frame, or the CPU converted payload made for other reasons. For DV GPU unpack it reads the raw r210 words.
- **No lock contention.** The render thread never waits for a scan. `TryTake` copies up to 16 ready observations of a few KB under the mutex, which costs under 0.02 ms.
- **Scans are not repeated per queued frame.** Each queued frame is scanned once and the result is cached in `QueuedFrame::subtitleBoxObservation`.
- **Copies of `SubtitleBoxPreview` are small.** It holds 8 observations, about 40-50 KB, and is copied a few times per frame on the render thread, about 0.01-0.02 ms.
- **No logging in the detector.** Worker `Trace()` builds a record even with no sink, which is sub-microsecond. `SUBTITLE PENDING` is logged for the first 4 only.
- **Scene and LED statistics are not shared with subtitle detection.** Of the overlaps the task listed, only bar finding was checked (item 8).

Cost when off: zero detection work. `type` = `off` and `classic` both leave `SubtitlePreviewMode::None`, so `SubtitlePreviewEnabled` is false. No `QueueSubtitleMeasurement`, no queue observations and no caption probes run, and the worker thread is never started (it starts on the first submit). Detection runs only for `black` and `generated_gray`. The only subtitle-module code still reached is `ExtractSubtitleBarEvidence` (about 0.3-0.8 ms), called by the crop path's `RecoverSubtitleInspectionBars` under its narrow trusted-bars conditions.

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

- `src/VP-V/modules/input/source_analysis/SourceResolutionDetector.cpp`
- `src/VP-V/modules/input/source_analysis/SourceResolutionDetector.h`
- `src/VP-V/modules/input/source_analysis/SourceResolutionMonitor.cpp`
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
- Listed under the tier: `intent_content`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#2-input](../../../../../docs/settings/VP-settings-reference.md#2-input).
- Fallback entry ("Fallbacks engaged"): `Source analysis: labels unknown (<reason>)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): MISSING.
- Isolation (fixed inputs, expected outputs, cost): SourceResolutionDetector tests; SceneDetector tests; subtitle corpus probes.
- Contract (formats, geometry, fallback entry): analysis reads the original frame even when re-base/repair are on.
- Chain (total delay at the 4/3/2-frame budgets): srcres latch + crossfade checks.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
