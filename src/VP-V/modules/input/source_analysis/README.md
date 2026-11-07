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
2. The worker runs `SourceResolution::Analyse` per tile and `ReadFrame`. It then updates three trackers (both axes, vertical, horizontal; the per-axis ones need twice the evidence), the live reading (median over about 2 s), the coherence-based confidence, the smoothed bitrate quality and effective bits, and the kernel votes (KernelInverse; see the scaling.rebase README). It writes the `Snapshot` and the SESSION-LONG history CSV ("source-resolution <yyyymmdd-hhmmss>.csv" beside the log, appended every few seconds; at 20 MB it continues in "...-002.csv", "-003" of the same session; the newest 5 sessions are kept, all their files; `SourceHistoryLog.h`). Its columns after `note` (per-axis frame readings, `detail_above`, `mixed`, `blank` / `cut` / `fade` flags, the 2:1 kernel, the classical snapshot behind the votes, the `sid_*` record) let the identity consensus be replayed from the file alone (`identity/SourceIdReplay.h`, `tools/eval/source_id/replay.py --srm`).
3. The host then pushes the selection and the setters from the active settings. In `auto` it calls `AutoFactors(width, height, source_resolution_auto_min_confidence, ...)`. Engaging waits for a cut, a real black, a fast pan or a low-detail moment (latch fixes), else crossfades after `source_resolution_engage_max_wait_s`. A return to native is immediate on a discontinuity, and when the declared reading ROSE above the re-based height - which, with the latch fixes, now takes SUSTAINED native evidence in the tracker (below), not one frame; it comes at the next cut (at most 5 s) when only the confidence fell or one reading is missing. The host turns the result into `rebaseFactor` / `rebaseFactorX` and, with the latch fixes, `rebaseFade` / `rebaseFromFactor(X)` from `AutoTransition`.
4. Look-ahead (`auto` only): the render loop passes up to 4 queued frames through `LookAheadFrame`, sampling luma through Bill's `AnalysisLumaSource`. The host passes the whole frame, not the active picture. A cheap 32 x 32-sample check per tile finds a cut or black ahead. While a change is pending, the first frame with picture after it is copied and analysed on the look-ahead worker, and `AutoFactor` engages exactly at that frame or cancels the change.
5. Other readers of `Read()`: the kernel for the re-base (`rebase_kernel_match`), VP deband and detail restore strength, low-bitrate handling, the residual grain guard, and the stats/status line (`Describe`). The host reports discontinuities on resync/source change, live queue reset (seek or skip) and file seek/step/rate change.

### Sustained native evidence (SID-B, doc 78 section 5)

With the latch fixes on (the default), the classical `SourceResolution::Tracker` clears or raises a DECLARATION only on sustained evidence (`Tracker::Settings::nativeEvidenceReadings` = 3 informative, non-mixed, non-starved native readings, covering `nativeEvidenceSeconds` = 1.5 s). The span of a run is its first reading's own interval plus the time to its newest reading, so k readings at the monitor's 0.5 s cadence cover k x 0.5 s (3 x 0.5 = 1.5 s: the defaults are exactly "3 readings"); the readings count at any cadence. The rules:

- One native frame (added noise raises DetailAbove, a sharp overlay or shot) no longer reverts a declaration; a reading below the declared value restarts the count.
- A mixed frame (the mixed-screen guard) is never native evidence: while a lower reading is declared it is neutral (neither clears nor raises it). With nothing declared it still restarts the gathering window, as before (no lower reading is gathered across a menu).
- A declared RISE (a reading above the declared value by more than `tolerance`) needs the same rule and goes to the lowest value of the sustained run.
- Discontinuities (`Discontinuity`, `Reset`: seek, resync, format change, a gap over 2 s, the clock going backwards) stay immediate.
- `Tracker::SustainedNative()` is the state the classical safety veto of the consensus is fed from (all three trackers sustained native with nothing gathered below).
- In the monitor, one missing reading no longer forces native at once (`AutoFactor` takes the pending path: native at the next cut or black, or after 5 s); a discontinuity still does.
- With the latch fixes off the behaviour is as before (one native frame reverts at once).

This is a VP-V behaviour change of VP's own classical path (not Bill's): a declared re-base now lasts about 1.5 s longer under native evidence, and a menu or panel over a declared film no longer reverts it on its first frame.

### Source identity mode (`source_id_consensus`: off / shadow / on)

`Monitor::SetSourceIdMode`. `off`: nothing runs, every value is byte-identical to before. `shadow`: the identity pipeline and consensus run, P14 and the CSV `sid_*` columns are published, one log line "source_id would-be: <field> <from> -> <to> (<reason>, support <s>)" and one CSV `would_be` row are written per consensus change - but `AutoFactor(s)`, `QualityView` and the re-base port use the classical decision, byte-identical to `off` (the record carries `kFlagShadow`; consumers must not act on it). Status line: "Source identity: shadow (classical decides)". `on`: the consensus decides (identity/README.md).

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
  - `SourceResolutionLatchTests` (SID-B: the sustained native rule on the tracker - `OneNativeFrameDoesNotRevertADeclaration`, `ThreeSustainedNativeReadingsRevertTheDeclaration`, `ALowerReadingRestartsTheNativeCount`, `MixedFramesNeverRevertADeclaration`, `ADeclaredRiseNeedsTheSameSustainedRule`, `DiscontinuitiesStayImmediate`, `SidBRuleIsOffWithTheLatchFixesOff`, `SidBDefaultsAreThreeReadingsOverOneAndAHalfSeconds`, `TrackerResumeAfterNativeEvidence`): the monitor through `AnalyseTilesForTest`. Real-footage cases need `VP_TEST_REAL_FOOTAGE` (or D:\hfr); `RealFilmCutDecisions` needs `VP_TEST_CUT_FILMS`.
  - `MixedScreenSourceResolutionTests`: the mixed-screen guard and the dense grid.
  - `SourceResolutionFrameBoundTests` and `SourceResolutionFrameBoundThreadedTests` (src/VideoProcessor-Test): frame-bound mode (real worker thread runs equal the simulated model, loss counting, order Discontinuity -> Frame -> AutoFactors; run-twice traces with randomised worker timing, source-time engage / fade / disengage, frozen source time with real sleeps, backward-step clamp, held results, held-queue overflow, era drop, `ReadyFor`, switch off = D 0). No footage needed.
- GPU (D3D11; WARP unless `VP_TEST_GPU=hardware`; only when GPU tests are allowed): `SourceResolutionHookTests` (moved from LibplaceboLutParserTests) methods `SourceResolutionMonitorIdentifiesThePlayerKernel`, `SourceResolutionLookAheadVerifiesOrCancelsTheSwitch`, `SourceResolutionMonitorDeclaresAutoEngagesAtACutAndDisengagesAtOnce`, `SourceResolutionReadsAndRebasesTheDolbyVisionTunnel` (filter `FullyQualifiedName~SourceResolutionHookTests`).
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

## Settings

- Slot keys (slots.json): `source_resolution`, `rebase_kernel_match`, `subtitle_fit`, `subtitle_reposition`, `subtitle_type`, `source_coding_fixes`
- As read today: `source_resolution` and `source_coding_fixes` are live, together with the ten `source_resolution_*` keys listed under "How the code works" (added to the slot list in this pass). `rebase_kernel_match` is read for the re-base, not by this slot's code. `subtitle_type` is the key `type` in `[vprenderer.subtitles.<Name>]` (Bill's subtitle code). `subtitle_fit` outside that section is discarded (fit follows `type`). `subtitle_reposition` (main config / command line) is parsed and ignored; repositioning is permanently disabled. Detection keys: see "How the code works".
- Planned keys: `label_detector`
- Tiers: not tiered; label-detector net optional

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `source_resolution` | input group (`[vprenderer.input.<Name>]`) | enum / int / string | native, auto, a height 144..8640, WIDTHxHEIGHT | auto | Input tab (settings reference section 2) | forced native | "Source resolution: not read (...)" on the stats line; auto stays native | same on all |
| `source_resolution_auto_min_confidence` | input group | float | 0..1 | 0.8 | Input tab | n/a (VP-only) | below it the classical reading does not engage | same on all |
| `source_resolution_evidence_shots` | input group | int | 1..10 | 3 | Input tab (Picture slider) | n/a (VP-only) | - | same on all |
| `source_resolution_evidence_seconds` | input group | float | 2..120 | 10 | Input tab (Picture slider) | n/a (VP-only) | - | same on all |
| `source_resolution_latch_fixes` | input group | bool | on, off | on | Input tab | forced off | off = the behaviour before, INCLUDING the pre-SID-B instant revert (the sustained native rule needs it on) | same on all |
| `source_resolution_mixed_guard` | input group | bool | on, off | on | Input tab | n/a (VP-only) | a mixed frame is native for new declarations and neutral for a declared one (SID-B) | same on all |
| `source_resolution_cut_fixes` | input group | bool | on, off | on | Input tab | forced off | needs latch fixes | same on all |
| `source_id_consensus` | input group (SID-5 plumbing pending: Monitor setter only) | enum | off, shadow, on | off (shadow after phase A, doc 78 D16) | not in Config yet (SID-5, P3 settings mixin) | forced off | "Source identity: off" / "Source identity: shadow (classical decides)" / "Source identity: classical only (<reason>)" | same on all (cost: the worker pipeline, a fraction of one core) |
| `source_id_abr_notice` | input group (SID-5 plumbing pending) | bool | on, off | on | not in Config yet (SID-5) | forced off | one-time notice `abr_quality_drop` | same on all |
| `Tracker::Settings::nativeEvidenceReadings` | code default (not a config key) | int | >= 1 | 3 (= the shots standard) | not in Config (derived: k readings; a Picture-slider candidate later) | n/a (VP-only) | - | same on all |
| `Tracker::Settings::nativeEvidenceSeconds` | code default (not a config key) | float | > 0 | 1.5 (= 3 readings x the 0.5 s cadence) | not in Config (derived) | n/a (VP-only) | - | same on all |
| `SourceIdentity::Settings::regimeEvaluations` | code default (not a config key) | int | >= 1 | 3 | not in Config (doc 78 D12) | n/a (VP-only) | - | same on all |
| `SourceIdentity::Settings::MinTimeBetweenChangesUs` (T_min) | derived (not a config key) | us | max(2 x dwell, minHoldAfterChange) | 10 s | not in Config (derived from the settings) | n/a (VP-only) | a held change reads "held: min hold" | same on all |
| `SourceHistory::kDefaultRotateBytes` / `Monitor::kHistorySessions` | code constants (not config keys) | bytes / int | - | 20 MB / 5 sessions | not in Config | n/a (VP-only) | - | same on all |

### Environment and diagnostics

| Name | Purpose | Default | Scope (test/debug/runtime) |
|---|---|---|---|
| `VP_TEST_SOURCE_MONITOR_DIGEST` | logs a digest of each tile readback | unset | test |
| `VP_SOURCE_ID_EVAL_DIR` | streaming evaluation reels (SourceIdStreamingEvalTests) | unset (skips) | test |
| `VP_SOURCE_ID_EVAL_FFMPEG` / `VP_SOURCE_ID_EVAL_ONLY` | ffmpeg path / reel filter of that evaluation | D:\tools\ffmpeg-full\bin\ffmpeg.exe / all | test |
| history CSV | `source-resolution <stamp>[-NNN].csv` beside vp.log; replay with `python -I tools/eval/source_id/replay.py --srm <file>` | on when the log folder is set | debug |

### Status lines (this module)

- Ctrl+I / Config: `Source identity: off`, `Source identity: shadow (classical decides)`, `Source identity: classical only (<reason>)`, `Source identity: consensus (tier N, level L)`.
- vp.log (shadow): `source_id would-be: <field> <from> -> <to> (<reason>, support <s>)`, one line per consensus change.
- Auto decisions with their reasons are in the history CSV (`auto` rows) and the stats line.

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
