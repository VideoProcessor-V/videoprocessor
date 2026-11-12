# Source analysis (`input.source_analysis`)

Sub-modules: [identity/](identity/README.md) (source-ID consensus, ABR step detector, quality-drop notice; header-only, not wired); [regions/](regions/README.md) (shared region evidence: grain split + block-statistics precursors, consumer maps derived on the fly; header-only, not wired).

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

VP owns two file pairs in this folder (moved by `git mv` in M2 step 10): `SourceResolutionDetector.h/.cpp` and `SourceResolutionMonitor.h/.cpp`. The subtitle, active-picture, scene and analysis-frame code the slot also uses is Bill's and stays in src/VideoProcessor-Lib (see "Bill's code the slot uses" below).

### Public API (VP)

- `SourceResolutionDetector.h`, namespace `SourceResolution` (pure CPU maths, standard library only):
  - `Analyse(luma, stride, n, Spectrum&, codeMaximum)`: one n x n tile (values 0..1) into a `Spectrum` (mirror-coherence curves per axis, coding measures, text band, sharp steps, periodic peak, bars, tile mean). `MeasureCoding` is the coding part alone.
  - `ReadFrame(tiles, Thresholds)`: the tiles of one frame into a `FrameReading` (ratio, per-axis `ratioV`/`ratioH`, informative/text/periodic/bar tiles, the mixed-screen guard's result, bitrate quality, effective bits, black). `Thresholds` carries `textBand`, `mixedGuard`, `codingFixes`, `latchFixes`, `blackLevel`, the tile centres and the signature tiles.
  - Helpers: `PlaceTiles` (the base 4 x 2 grid, plus two rows when dense), `TileSize` (with the latch fixes: 128 at 1080, 256 at 2160), `Informative`, `FindPeak`, `DetailAbove`, `AxisFrame`, `SnapRatio`, `PatternChange`, `Describe`.
  - `Tracker`: the conservative declared value. It needs `minimumShots` and `minimumSeconds` of agreeing evidence (defaults 3 shots, 10 s; set by `SetEvidence`). Also `SetLatchFixes`, `SetCutFixes`, `Update`, `Discontinuity`, `Ratio`, `Candidate`, `Cuts`. `Tracker::CutJudge` and `FrameCutJudge` are the cut judges.
- `SourceResolutionMonitor.h`, class `SourceResolutionMonitor::Monitor` (needs libplacebo):
  - Render thread: `Frame(gpu, dispatch, source, sequence, seconds)`; `AutoFactor` / `AutoFactors` (the factor `source_resolution: auto` wants, per axis); `AutoTransition` (the crossfade weight and the factors being left); `LookAheadFrame` (queued frames ahead); `SetSelection`, `SetMotion`; the setters `SetTextGuard`, `SetMixedGuard`, `SetCodingFixes`, `SetLatchFixes`, `SetCutFixes`, `SetEngageTiming`, `SetEvidence`, `SetHistoryFolder`; `Release`.
  - Any thread: `Read()` (a `Snapshot` copy under the mutex), `Discontinuity(cause)`, `AutoRatio()`, `Selection()`, `Ahead()`.
  - Frame-bound mode (resync Q3, default off; the behaviour with it off is unchanged): `SetFrameBound(on, leadFramesD)`, `FrameBound()`, `FrameLead()`, `ReadyFor(s)`, `AnalysedThrough()`, `NoteWaitMissed()`, and the counters `HeldDropped()`, `WaitsMissed()`, `ClockStepsBack()`, `HeldCount()`. See "Frame-bound mode" below.
  - Tests: `AnalyseTilesForTest` (synchronous, no GPU), `InjectResultForTest` and `NoteSubmittedForTest` (frame-bound tests), `FadeWeight`, `FadeMixTimestamps`, `FadeMixValue`.
  - Free function `Describe(snapshot, autoRatio, autoSelected)`: the status text.
- `VpFrameAnalysis.h` (C9 commit A, doc 105 phase 4a; the frame's luma view and the P1 engine reader):
  - `SourceViewOf(source, view)`: the frame's decoded luma as the lane reads it (10-bit sampler codes, msbShift 6, stride = width; `keepAlive` holds the plane): the shared plane, or a pooled plane decoded here for P010 / P210 / decoded DV. False when no plane is ready. The source frame tap delivers exactly this view.
  - `LiveEngine8(source, out, hw, hh)`: the P1 engine half through the shared decode (the live fill): `DecodeSharedLuma`, `SourceViewOf`, `HalfLumaRows`, `Engine8Half`. False without a 10-bit view (`Engine8Half` is exact for msbShift 6 only).
  - `P1EngineViewDiffersFromProducer(source)`: the named exception (RGB structure luma; P010 / P210 / v210 / UYVY at full range). Commit B keeps the producer's own decode for these unless EPE evidence shows otherwise (doc 91 D C9).
  - `P1Reader` (implements `VpSkeleton::IP1Source`, `core/LaneInterfaces.h`): `ReadEngine8(seq, source, out, hw, hh, how)` returns the lane's published P1 under the slot seqlock (`PublishedEngine8Of`, `core/AnalysisLane.h`), else the live fill (`how`: `LanePublished` / `LiveFill`). Counters: `LanePublished`, `FilledByMotion` (p1FilledByMotion), `SeqlockRetries` (recycled copies), `Unread`. One reader per calling worker.

### Frame-bound mode (resync Q3)

`SetFrameBound(true, D)` makes the monitor's decisions a function of the frame sequence alone, so the decisions are the same whatever the worker's timing **as long as no analysis is lost** (what is proven: the CPU tests below, with a simulated and a real worker thread and randomised completion delays, when every submitted frame is analysed). Analyses can still be lost to timing, and the monitor counts and shows every loss instead of hiding it: a full job queue drops the oldest (`jobs dropped`), a full held ring drops the oldest (`dropped`), a frame whose tile copy finds both GPU readback slots in flight is skipped (`copies skipped`, GPU timing), and the lane's bounded wait can run out (`waits missed`). `Reproducible()` is false after any of them and `Describe` then says "this run is not reproducible". The GPU slot skip cannot be tested without a GPU (Q5 covers it):

- The decision for render sequence `s` (`AutoFactor` / `AutoFactors` `renderSequence`) uses only the analysis of frames <= `s - D`. A result for a later frame is held in a fixed ring of `kHeldCap` (16); on overflow the oldest is dropped and counted (`Describe` shows held / dropped). The released result becomes what `Read()` returns; the dense grid for the next copy follows the released result too. Jobs reach the worker through a FIFO of `kJobQueueCap` (4), not by replacing the pending copy.
- Every time input is SOURCE time passed by the caller (frame timestamp / measured rate, never a constant): `Frame`, `AutoFactor(s)`, `AutoTransition` and the job `seconds`. The crossfade, the engage wait (`source_resolution_engage_max_wait_s`), the pan clock, the 5 s disengage and the failure pause use it; the steady clock is not read for any decision. A backward step (a seek) is clamped on the monitor's clock (it holds, then goes on) and counted (`ClockStepsBack`).
- Wall clock that remains, display or idle only: `analyseMs` (steady_clock around one analysis), the history CSV's row time and file name (`GetLocalTime`), the history flush interval, the worker's condition-variable wait (no timeout).
- `Discontinuity()` bumps an era counter and takes effect at the render thread's next `Frame` / `AutoFactor` (call it from that thread, in frame order); held results and queued jobs of an older era are dropped (`heldStale`). A reset made by the analysis itself (gap, tracker discontinuity) travels with its result and applies when that result is released.
- The look-ahead verdict and the look-ahead evidence for the cut judge are not used (they are analysis of frames after the one decided).
- `ReadyFor(s)` is true when nothing is in flight (copied, queued or being analysed) for frames <= `s - D`; `AnalysedThrough()` is the last analysed sequence. The analysis lane's bounded wait (on deterministic runs only, never a late present) polls `ReadyFor` and calls `NoteWaitMissed()` when the bound runs out. `AnalysisLane::SetReadyProbe(ready, onMissed)` is that wait (host wiring: `ReadyFor` and `NoteWaitMissed`, Q1). `FrameFrontForTest` runs Frame()'s frame-bound front part (clock + release) without a GPU so the renderer's order Discontinuity -> Frame -> AutoFactors is testable; a reset consumed by Frame is carried to AutoFactor. `SubmitJobForTest` hands tiles to the real worker thread through the same bounded FIFO.

### Data flow (VP)

1. LibplaceboVideoRenderer.cpp (render thread, once per new source frame) calls `Frame` with the native, unwarped, not re-based frame (`renderImage`). Every `intervalSeconds` (0.5 s) a compute shader copies 8 tiles (16 with the dense grid) of plane 0, inside the crop, into one of two readback buffers. Finished copies are collected without waiting (`pl_buf_poll(..., 0)`) and handed to the worker; a newer copy replaces one the worker has not started. A change of the source signature (size, system, levels, bits, transfer, primaries) is a discontinuity. Dolby Vision is read on its decoded I plane.
2. The worker runs `SourceResolution::Analyse` per tile and `ReadFrame`. It then updates three trackers (both axes, vertical, horizontal; the per-axis ones need twice the evidence), the live reading (median over about 2 s), the coherence-based confidence, the smoothed bitrate quality and effective bits, and the kernel votes (KernelInverse; see the scaling.rebase README). It writes the `Snapshot` and the SESSION-LONG history CSV ("source-resolution <yyyymmdd-hhmmss>.csv" beside the log, appended every few seconds; at 20 MB it continues in "...-002.csv", "-003" of the same session; the newest 5 sessions are kept, all their files; `SourceHistoryLog.h`). Its columns after `note` (per-axis frame readings, `detail_above`, `mixed`, `blank` / `cut` / `fade` flags, the 2:1 kernel, the classical snapshot behind the votes, the `sid_*` record) let the identity consensus be replayed from the file alone (see "History CSV" below).
3. The host then pushes the selection and the setters from the active settings. In `auto` it calls `AutoFactors(width, height, source_resolution_auto_min_confidence, ...)`. Engaging waits for a cut, a real black, a fast pan or a low-detail moment (latch fixes), else crossfades after `source_resolution_engage_max_wait_s`. A return to native is immediate on detail above the re-based height or a discontinuity, and at the next cut (at most 5 s) when only the confidence fell. The host turns the result into `rebaseFactor` / `rebaseFactorX` and, with the latch fixes, `rebaseFade` / `rebaseFromFactor(X)` from `AutoTransition`.
4. Look-ahead (`auto` only): the render loop passes up to 4 queued frames through `LookAheadFrame`, sampling luma through Bill's `AnalysisLumaSource`. The host passes the whole frame, not the active picture. A cheap 32 x 32-sample check per tile finds a cut or black ahead. While a change is pending, the first frame with picture after it is copied and analysed on the look-ahead worker, and `AutoFactor` engages exactly at that frame or cancels the change.
5. Other readers of `Read()`: the kernel for the re-base (`rebase_kernel_match`), VP deband and detail restore strength, low-bitrate handling, the residual grain guard, and the stats/status line (`Describe`). The host reports discontinuities on resync/source change, live queue reset (seek or skip) and file seek/step/rate change.

### History CSV (SID-B0)

`SourceHistoryLog.h/.cpp` (C++14, Win32 file calls with wide paths): `Append` only queues a row in a bounded in-memory queue under a short lock and never waits on the disk (it is called from the render thread too); the analysis worker flushes (`FlushHistory`), appending. `Open` does no disk work either: the first flush creates the folder, deletes the earlier sessions beyond the newest 4, and creates the base file with the header and the session row. At `kDefaultRotateBytes` (20 MB) it continues in the next numbered file of the same session (each starts with the header; a row never straddles files). A failed write or dropped rows (queue bound `kDefaultQueueRows` 20000: the OLDEST rows are dropped, counted) are reported once as a log line and stay on the stats line (`Snapshot::historyNote`) while they last. Free-text notes are CSV-quoted and every row (also session / auto / discontinuity / would_be rows) has the full 39 columns. Columns after `note`: `mode, t_src, seq, frame_ratio_v, frame_ratio_h, detail_above, mixed, blank, cut, fade, apl, axis_x, axis_y, axis_conf, kernel_id, kernel, kernel_votes, kernel_agree, frame_quality, frame_noise, sid_tier, sid_level, sid_ratio_h, sid_ratio_v, sid_reason`; `mode` is the source-identity mode (off / shadow / on), the `sid_*` columns are filled while the pipeline runs, and `detail_above` is the classical safety veto flag as the consensus saw it (after the N2 per-axis exemption: 0 when both axes were exempt). Behaviour changes (VP's own file, not Bill's): the file is appended, not rewritten (the whole session is kept, not the newest ~10 minutes); a session may span several files; rows have 25 more columns.

### Chain estimator and the consensus (SID train r2)

`Monitor::SetChainEnabled(true)` (default OFF: nothing runs, every value is byte-identical to before) makes the monitor own a `ChainEstimator` (`identity/chain/`): `Frame()` feeds it the frame clock and the active rectangle (a leaf lock), the analysis worker its cuts, blanks, discontinuities and the classical decision; it follows `SetSourceIdMode` (Off / Shadow / On) and `SetOriginalPathway` (forces Off). The estimator publishes the P14g record (`GeometryView()` reads it: `Confident()` / `Provisional()`); the pipeline reads it as the chain voter of the consensus. The chain has no latch, no engage gate and no hold of its own: the CONSENSUS decides WHAT (`AutoFactor` / `AutoFactors` use the consensus ratio in place of the tracker's declared value when On, N1) and the original latch decides WHEN (engage moments, dwell, crossfade, revert paths). With the consensus On, a native-looking frame that confident chain evidence contradicts neither clears the trackers (`Tracker::SetChainContradictsNative`) nor raises the classical safety veto (N2). `Monitor::NativeFramesHeldByChain()` counts the frames left out. See [identity/README.md](identity/README.md) and [identity/chain/README.md](identity/chain/README.md).

### Threading (VP)

- Two threads per `Monitor`, started in the constructor: `Worker` (analysis, trackers, kernel fits, history file) and `AheadWorker` (look-ahead target frames).
- Locks: `mutex` (snapshot, trackers, selection, history), `aheadMutex` (look-ahead state and tiles), `frameJudgeMutex` (the per-frame cut judge). The settings flags are atomics.
- Render thread: `Frame` (GPU submit and readback collection; never waits for the GPU), `AutoFactors`, `AutoTransition`, the setters and `LookAheadFrame`. `LookAheadFrame` copies the target frame's tiles through a `std::function` call per sample, under `aheadMutex`.
- The lock as the code stands here: `Analyse` holds `mutex` from the tracker updates through both `KernelInverse::FitTile` calls and the snapshot write. The render thread takes the same mutex several times per frame (`SetSelection`, `Read()` in `AutoFactor`/`AutoFactors`, the kernel read, the deband/detail reads). So while a 2:1 reading is declared or gathered it can wait for the fits (up to about 50 ms). The fix (fits outside the lock) is on branch perf/byte-identical (queue item 165) and is not merged here.

### Config keys actually read (VP)

All are validated as `input`-group keys (RendererProfileConfig.h; sections `[vprenderer.input.<Name>]`, flattened into the renderer profile) and read per active profile in `ApplyDisplayRuleOverrides` (LibplaceboVideoRenderer.cpp). Defaults from `RendererSettings`:

- `source_resolution`: `native` / `auto` / a height (144..8640) / `WIDTHxHEIGHT`; default `auto`. An unrecognised value is ignored without a log line.
- `source_resolution_auto_min_confidence` (0..1, default 0.8), `source_resolution_evidence_shots` (1..10, default 3), `source_resolution_evidence_seconds` (2..120, default 10).
- `source_resolution_text_guard`, `source_resolution_mixed_guard`, `source_coding_fixes`, `source_resolution_latch_fixes`, `source_resolution_cut_fixes`: `on` / `off`, all default `on`.
- `source_resolution_engage_max_wait_s` (0..600, default 20), `source_resolution_crossfade_s` (0..10, default 1.5), `source_resolution_release_fade_s` (0..0.25, default 0.25).
- `rebase_kernel_match` is read by the host for the re-base (scaling.rebase). The monitor fits kernels whatever it says.
- The Original pathway forces `source_resolution: native`, `rebase_kernel_match: off`, `source_coding_fixes: off`, `source_resolution_latch_fixes: off` and `source_resolution_cut_fixes: off` (OriginalPathway.h).
- `label_detector` is not read anywhere (planned).
- Test only: the environment variable `VP_TEST_SOURCE_MONITOR_DIGEST=1` logs a digest of each tile readback.

### User-visible fallbacks / notices (VP)

- No "Fallbacks engaged" entry. The planned `Source analysis: labels unknown (<reason>)` is never produced.
- A GPU-side failure of the tile copy (buffer, shader, dispatch, readback) is logged once per cause and retried after 60 s. While nothing is read the status/stats line shows "Source resolution: not read (...)", and `auto` stays native. A source that cannot be sampled shows the same way.
- The look-ahead's cut judge assumes 24 Hz until the frame interval is measured: log only ("Rate fallback: source-resolution cut judge ...").

### Dependencies to lift it

- `SourceResolutionDetector.*`: the standard library and the project `pch.h`; compiled into VideoProcessor-Lib.
- `SourceResolutionMonitor.*`: libplacebo (dispatch, buffers), `KernelInverse` (`<VP-V/modules/scaling/rebase/KernelInverse.h>`), `DebugLog.h`, `GeneralRateLog.h` and Windows `GetEnvironmentVariableA`. Compiled into the renderer DLL (VideoProcessor-VPRenderer) and the test project. The look-ahead's caller needs Bill's `AnalysisLumaSource` / `PrepareFrameAnalysis`.
- The forwarding header src/VideoProcessor-Lib/vprenderer/SourceResolutionMonitor.h keeps the host's include building until M2 step 0.

### Bill's code the slot uses (not owned by VP)

These stay in src/VideoProcessor-Lib and VP does not change them (owner principle 2026-10-24):

- Subtitle detection: `SubtitleBoxDetector`, `SubtitleBarEvidence.h`, `SubtitleOpaqueCardRefinement.h`, `SubtitleBoxLookahead.h` (the current frame plus up to 7 queued frames, `MaxFrames` = 8), `SubtitleTemporalFill.h`, and the worker `SubtitleMeasurementWorker.h`. Scans run on one CPU worker thread; the render thread never waits for one. Driven by the `[vprenderer.subtitles.<Name>]` profile: `type` (`off` / `classic` / `generated_gray` / `black`; detection runs only for `black` and `generated_gray`), `subtitle_script_hint`, `subtitle_near_bar_px` (0..200, default 20), `subtitle_hold_ms` (0..1000, default 250), `cleanup_fill`. It registers no "Fallbacks engaged" entry. `subtitle_reposition` (main config / command line) is parsed and ignored: repositioning is disabled.
- `ActivePictureAnalyzer` (active picture / crop), `SceneDetector` (cut/fade/black), `AnalysisFrame` (analysis-frame preparation shared by live and queued analysis).

### Test it standalone

Build x64 Release, then run single-threaded from the repo root, one class at a time, and read the TRX:

```
vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~SourceResolutionDetectorTests" /Logger:"trx;LogFileName=source_analysis.trx"
vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~SourceResolutionLatchTests" /Logger:"trx;LogFileName=source_latch.trx"
vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~MixedScreenSourceResolutionTests" /Logger:"trx;LogFileName=source_mixed.trx"
```

- CPU only:
  - `SourceResolutionDetectorTests`: detector and tracker. `RealFootageReadings` needs `VP_TEST_REAL_FOOTAGE`, `RealCodecReadings` needs `VP_TEST_CODEC_CORPUS`; else skipped.
  - `SourceResolutionLatchTests`: the monitor through `AnalyseTilesForTest`. Real-footage cases need `VP_TEST_REAL_FOOTAGE` (or D:\hfr); `RealFilmCutDecisions` needs `VP_TEST_CUT_FILMS`.
  - `MixedScreenSourceResolutionTests`: the mixed-screen guard and the dense grid.
  - `SourceResolutionAbsenceHoldTests` (SID-B0): pins that a declared lower reading is HELD by frames with no evidence (no informative tile, no tiles, dark / low-contrast and blurred starved frames) and cleared by a genuine native reading or a discontinuity. Tracker level (hand-made FrameReadings, not Monitor / AutoFactor), no footage. An informative MIXED screen reading native is not held today - it clears the declaration at once (the incident path, deliberately not pinned).
  - `SourceResolutionFrameBoundTests` and `SourceResolutionFrameBoundThreadedTests` (src/VideoProcessor-Test): frame-bound mode (real worker thread runs equal the simulated model, loss counting, order Discontinuity -> Frame -> AutoFactors; run-twice traces with randomised worker timing, source-time engage / fade / disengage, frozen source time with real sleeps, backward-step clamp, held results, held-queue overflow, era drop, `ReadyFor`, switch off = D 0). No footage needed.
- GPU (D3D11; WARP unless `VP_TEST_GPU=hardware`; only when GPU tests are allowed): `SourceResolutionHookTests` (moved from LibplaceboLutParserTests) methods `SourceResolutionMonitorIdentifiesThePlayerKernel`, `SourceResolutionLookAheadVerifiesOrCancelsTheSwitch`, `SourceResolutionMonitorDeclaresAutoEngagesAtACutAndDisengagesAtOnce`, `SourceResolutionReadsAndRebasesTheDolbyVisionTunnel` (filter `FullyQualifiedName~SourceResolutionHookTests`). Owner-approved change (waiver W1, relayed 2026-11-07): the history-retention assertion of `SourceResolutionMonitorDeclaresAutoEngagesAtACutAndDisengagesAtOnce` calls `FlushHistoryNow()` first and expects 5 files (the newest 4 earlier sessions plus this one) instead of 4, because the session start (folder, retention, the base file) now happens on the first flush, never on the render thread that calls `SetHistoryFolder`. Waiver W2 (`tools/test_check_shared_units.py` expects `U21`, the registration of unit SourceGeometryView) is owner-approved too.
- Bill's subtitle code: `SubtitleBoxDetectorTests`, `SubtitleBoxLookaheadTests`, `SubtitleOpaqueCardRefinementTests`, `SubtitleMeasurementWorkerTests`; probes in tools/subtitle_*_probe; exact native frame capture with `VP_SUBTITLE_CAPTURE_PATH` (see AGENTS.md).

## Cost

This section covers the subtitle code, which is **Bill's** (upstream `v1.4.02-beta`). It stays at its path and VP does not change it
(owner principle 2026-10-24). The cost of VP's own source-resolution monitor (tile copies, analysis, kernel fits, the lock) is reviewed in the scaling.rebase README, "Cost". The notes below describe how VP drives it and what that costs. Changes
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

## Tuning (not in Config)

Code defaults of the history CSV that are not config keys (each fixed in code):

| Name | Meaning | Default |
|---|---|---|
| `SourceHistory::kDefaultRotateBytes` | the history CSV rotates to the next numbered file | 20 MB |
| `Monitor::kHistorySessions` | history sessions kept (all their files) | 5 |
| `SourceHistory::kDefaultQueueRows` | in-memory rows between two flushes; beyond it the oldest are dropped, counted and reported | 20000 |

## Environment and diagnostics

| Name | Purpose | Default | Scope (test/debug/runtime) |
|---|---|---|---|
| history CSV | `source-resolution <stamp>[-NNN].csv` beside vp.log (the replay input) | on when the log folder is set | debug |
| `VP_SOURCE_ID_CONSENSUS` / `VP_SOURCE_ID_EVIDENCE` | read once by the Monitor constructor: `shadow` / `on` sets `source_id_consensus` and switches the chain estimator on; `off`, `log`, `log+lines`, `log+lines+frames` for the evidence log (see identity/chain/README.md) | unset | test / debug |

## Status lines (history CSV)

- Chain estimator (while enabled): `Snapshot::chainNote` is appended to the stats line (`Source identity: ... confident / unsure (<reasons>) ...`).
- `p1FilledByMotion` (C9, planned for the 4b consumer): `P1Reader::FilledByMotion()` frames the motion producer filled live because the lane had not published P1 for them (target 0 in steady state once the lane covers the full queue depth), with `SeqlockRetries` and `Unread`; shown on Ctrl+I and Config status when 4b wires the reader. Not on any status line yet (commit A has no consumer).
- A failed history write or dropped rows: `source history CSV: write failed (<reason>)` / `source history CSV: rows dropped: N` (one log line each, and the stats line while it lasts).

## Settings

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `active_picture_startup_hold` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | on | VP-V > Proposed retirement > Proven fixes (all on by default) | Original pathway sets `off` (ADR-0019) | Until the first crop decision of a queue generation the output is black, or the remembered crop of the same source (StartupCropHold); presentation only. proposed for retirement (group A) | same on all (not tiered) |
| `frame_analysis_store` | [vpv] (global; also read from the profile) | bool | on, off | off | VP-V > Pipeline (expert) (stored in [vpv]) | Original pathway sets `off` (ADR-0019) | VP's per-source-frame analysis record plus one shared luma decode read by every analyser (WP-23); off until the big test; never used on the Original pathway | same on all (not tiered) |
| `intent_content` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | off | VP-V > Input > Main > Signal and content; also VP Renderer > Rendering > Experimental content-adaptive settings (upstream copy greyed while VP-V is on) | Original pathway sets `off` (ADR-0019) | Content recognition (experimental): a rule-based class (film, video, sports, animation, still menu) for the profile rules' `content` variable, taken after intent_content_hold_seconds at a cut. status line: Source analysis: labels unknown (<reason>) | same on all (not tiered) |
| `intent_content_hold_seconds` | profile ([vprenderer.Default] and matching profiles) | float | 1.0 - 60.0 | 5 | VP-V > Input > Advanced > Content labels: tuning | n/a (VP-only): unchanged by the Original pathway | Seconds a newly recognised content class must hold before it is taken (at a cut) | same on all (not tiered) |
| `source_coding_fixes` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | on | VP-V > Proposed retirement > Proven fixes (all on by default) | Original pathway sets `off` (ADR-0019) | Coding readings (effective bits, blocking, banding) made robust to upscales, dither and noise; off = the fixed 8/16-px grid and one-pixel occupancy. proposed for retirement (group A) | same on all (not tiered) |
| `source_resolution` | profile ([vprenderer.Default] and matching profiles) | string | native, auto, a height in lines 144 - 8640, or WIDTHxHEIGHT (width 144 - 17280, height 144 - 8640) | auto | VP-V > Scaling > Advanced > Set by the Scaling tier | Original pathway sets `native` (ADR-0019) | Re-base: many 4K streams are lower resolution underneath, upscaled by the player. auto detects it, native = as captured, a height (or WIDTHxHEIGHT, a source upscaled per axis) forces it; VP shrinks the picture back to its real size and upscales with its own path | Scaling Classic=native; Scaling Low=auto; Scaling Mid=auto |
| `source_resolution_auto_min_confidence` | profile ([vprenderer.Default] and matching profiles) | float | 0.0 - 1.0 | 0.8 | VP-V > Scaling > Advanced > Re-base evidence | n/a (VP-only): unchanged by the Original pathway | Confidence auto needs before it re-bases | same on all (not tiered) |
| `source_resolution_crossfade_s` | profile ([vprenderer.Default] and matching profiles) | float | 0.0 - 10.0 | 1.5 | VP-V > Scaling > Advanced > Re-base timing | n/a (VP-only): unchanged by the Original pathway | Length (s) of the crossfade used when no switching moment came within the maximum wait | same on all (not tiered) |
| `source_resolution_cut_fixes` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | on | VP-V > Proposed retirement > Proven fixes (all on by default) | Original pathway sets `off` (ADR-0019) | Cut judging for the latch: camera moves, scrolls, dark passes and flashes are not cuts, same-level shot / reverse-shot cuts are, and shots also need part of the evidence time. proposed for retirement (group A) | same on all (not tiered) |
| `source_resolution_engage_max_wait_s` | profile ([vprenderer.Default] and matching profiles) | float | 0.0 - 600.0 | 20 | VP-V > Scaling > Advanced > Re-base timing | n/a (VP-only): unchanged by the Original pathway | The longest wait (s) for a switching moment (a cut, real black, fast pan or low-detail moment) before auto crossfades into the re-base | same on all (not tiered) |
| `source_resolution_evidence_seconds` | profile ([vprenderer.Default] and matching profiles) | float | 2.0 - 120.0 | 10 | VP-V > Scaling > Advanced > Re-base evidence | n/a (VP-only): unchanged by the Original pathway | Seconds of agreeing evidence auto needs before it declares a lower source (Picture slider; Standard 10) | same on all (not tiered) |
| `source_resolution_evidence_shots` | profile ([vprenderer.Default] and matching profiles) | int | 1 - 10 | 3 | VP-V > Scaling > Advanced > Re-base evidence | n/a (VP-only): unchanged by the Original pathway | Separate shots of agreeing evidence auto needs before it declares a lower source (Picture slider; Standard 3) | same on all (not tiered) |
| `source_resolution_latch_fixes` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | on | VP-V > Proposed retirement > Proven fixes (all on by default) | Original pathway sets `off` (ADR-0019) | Latch fixes for source_resolution auto: re-engage on the standard after native evidence, level-aware native readings, per-tile floor test, shape cuts, the switch at a cut / real black / fast pan / low-detail moment or a crossfade, a short release fade; off = the earlier behaviour and no fades. proposed for retirement (group A) | same on all (not tiered) |
| `source_resolution_mixed_guard` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | on | VP-V > Scaling > Main > Re-base (your picture slider); also VP Renderer > Rendering > Fixes under test (upstream copy greyed while VP-V is on) | n/a (VP-only): unchanged by the Original pathway | A mixed screen (a native UI with a lower-resolution video in it, a 4K overlay, picture-in-picture) is read as native: no whole-frame re-base. status line: re-base precision: <the re-base's float-intermediate note> | same on all (not tiered) |
| `source_resolution_release_fade_s` | profile ([vprenderer.Default] and matching profiles) | float | 0.0 - 0.25 | 0.25 | VP-V > Scaling > Advanced > Re-base timing | n/a (VP-only): unchanged by the Original pathway | Length (s) of the fade back to native when evidence of native resolution returns (at most 0.25 s) | same on all (not tiered) |
| `source_resolution_text_guard` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | on | VP-V > Scaling > Main > Re-base (your picture slider); also VP Renderer > Rendering > Fixes under test (upstream copy greyed while VP-V is on) | n/a (VP-only): unchanged by the Original pathway | Tiles that hold overlay text (the player's own subtitles, drawn at the output resolution) are no evidence for source_resolution auto. status line: re-base precision: <the re-base's float-intermediate note> | same on all (not tiered) |


- Slot keys (slots.json): `source_resolution`, `rebase_kernel_match`, `subtitle_fit`, `subtitle_reposition`, `subtitle_type`, `source_coding_fixes`
- As read today: `source_resolution` and `source_coding_fixes` are live, together with the ten `source_resolution_*` keys listed under "How the code works" (added to the slot list in this pass). `rebase_kernel_match` is read for the re-base, not by this slot's code. `subtitle_type` is the key `type` in `[vprenderer.subtitles.<Name>]` (Bill's subtitle code). `subtitle_fit` outside that section is discarded (fit follows `type`). `subtitle_reposition` (main config / command line) is parsed and ignored; repositioning is permanently disabled. Detection keys: see "How the code works".
- Planned keys: `label_detector`
- Tiers: not tiered; label-detector net optional

## Fallbacks

- "Fallbacks engaged" entry: Source analysis: labels unknown (<reason>). Planned, NOT implemented: no labels are produced and nothing engages it.
- Behaviour (planned): Low confidence -> 'unknown' labels (A is trained with ~12% unknown); un-identified kernel -> Lanczos re-base or no re-base.
- Today: no entry. Monitor GPU failures show only on the status/stats line; subtitle detection (Bill's) has none (see "User-visible fallbacks / notices").

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

Code location: **partly migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

VP code in this folder:

- `src/VP-V/modules/input/source_analysis/AnalysisLumaDomain.h` (DIV-AN-01 limited-domain logic of `AnalysisLumaSource`: coefficients, full-to-limited maps, domain queries; header-only, included at the end of Bill's AnalysisLumaSource.h)
- `src/VP-V/modules/input/source_analysis/SourceResolutionDetector.cpp`
- `src/VP-V/modules/input/source_analysis/SourceResolutionDetector.h`
- `src/VP-V/modules/input/source_analysis/SourceResolutionMonitor.cpp`
- `src/VP-V/modules/input/source_analysis/SourceResolutionMonitor.h`

Also listed (used, owned elsewhere):

- `src/VP-V/modules/scaling/rebase/KernelInverse.cpp` (scaling.rebase; the monitor's kernel fits)
- `src/VideoProcessor-Lib/ActivePictureAnalyzer.cpp` (Bill's)
- `src/VideoProcessor-Lib/SceneDetector.cpp` (Bill's)
- `src/VideoProcessor-Lib/SubtitleBoxDetector.cpp` (Bill's)
- `src/VideoProcessor-Lib/AnalysisFrame.cpp` (Bill's)

Forwarding header at the legacy path (removed at M2 step 0; new code includes the `<VP-V/...>` path):

- `src/VideoProcessor-Lib/vprenderer/SourceResolutionMonitor.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0004 in [docs/decisions](../../../../../docs/decisions/README.md)

## Analysis lane products (skeleton S6/S7, `lane/`)

Header-only producers for the analysis lane (`src/VP-V/core/AnalysisLane.h`), off unless `analysis_lane` is on:

- `LaneCpuProducts.h`: P1 half/quarter luma and P4 source stats (CPU, chunked, bit-identical to the one-shot live-fill form), `ClassifyEvents` (cut/fade/flash derived per consumer, never stored) and the `source_stats_walk` either/or (`original` = Bill's walk, the default).
- `LaneGpuJobs.h`: `PlanChunks` and the generic `GpuComputeLaneJob` for the GPU-compute products (P3/P5/P6), dispatched by the host's `IGpuDispatcher`.
- `LaneReadback.h`: `ReadbackRing`, non-blocking P5 readback (staging copy, event query, Map with DO_NOT_WAIT).

Tests: `src/VP-V/tests/LaneProductsTests.cpp`.

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `source_resolution`, `source_resolution_auto_min_confidence`, `source_resolution_evidence_shots`, `source_resolution_evidence_seconds`, `source_resolution_text_guard`, `source_resolution_mixed_guard`, `source_resolution_latch_fixes`, `source_resolution_cut_fixes`, `source_resolution_engage_max_wait_s`, `source_resolution_crossfade_s`, `source_resolution_release_fade_s`, `subtitle_fit`, `subtitle_reposition`, `subtitle_type`, `source_coding_fixes`, `frame_analysis_store`.
- Planned keys (not yet implemented): `label_detector`, `source_id_abr_notice`, `source_id_consensus`.
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

`SourceRingPin.h` (P13-DECL, C++14, header only): the PIN interface (`IFramePin`), the no-op host that refuses visibly ("source ring not bound"), and `FramePinBook`, the bookkeeping of at most 3 pinned source sequences inside the derived window [N - back, N + lead] (`back`/`lead` are passed in, never constants). The S7 queue facts Flush / Discontinuity / FormatChange invalidate every pin (a reset is reported by its producer as Discontinuity; refs/110 :33-43, 99c A3 item 5). The reduced tier's "4 frames + 3 pins, luma only" reserve (`core/ReserveRows.h`) is a lane choice and a sizing ceiling, not a depth constant; confirmed by the TD round-3 measurement (99c A3 item 2). Declaration only: nothing in the renderer calls it and it holds no frames (contract: `core/RingContract.h`; tests: `SourceRingPinTests.cpp`).
