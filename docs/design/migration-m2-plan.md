# M2 migration plan: the modules early migration could not move (2026-10-24)

Follows [migration-early-inventory.md](migration-early-inventory.md) (what moved early, and why the
rest is BLOCKED) and [migration-path-map.md](migration-path-map.md) (old to new paths). Rules: ADR-0017
and `D:\codex\vp-handover\04-workflow.md`. That means one module per commit, `git mv`, the module's
`live_files` and `components-master.json` in the same commit, no duplicate source,
`python tools/check_module_manifests.py` green (this now includes the old-path check), and the
build and tests green.

Estimates are **agent wall-clock hours**. Each covers the mechanical move, an incremental x64
Release build (about 10-15 min) and its share of the verification below. They are not
human-developer time.

## Why these modules wait for M2

1. **`VideoProcessor-Test.vcxproj.vcxproj`** compiles most `vprenderer/*.cpp` units directly. While
   parallel branches change that project file (net-runner, vpscalerender-motion and dv-l8 did on
   2026-10-24), moving any of those `.cpp`s means a project-file edit that collides with them.
2. **Active code.** B3, timing and queue work; de-judder and motion work; DV work; the Config and
   settings work.
3. **Includers on active branches.** `LibplaceboVideoRenderer.cpp`/`.h`, `ConfigEditorWindow.cpp`,
   `IRenderer.h`, `MainConfigSchema.h` and `RendererCaptureHarnessTests.cpp` include almost
   everything. The early moves left 15 forwarding headers for them.

After M1, once every feature branch has merged and no branch is open against the test project,
blocker 1 disappears. Blockers 2 and 3 go away as each area's work lands. M2 is a short freeze
window: no feature branches open while it runs.

## Order

Leaves first, timing and motion last. Each step is one commit unless noted. **CPU** means the full
CPU suite (single-threaded, read from the TRX). Run it at the marked checkpoints, not after every
commit (the moves are mechanical), and bisect a failing range. **GPU** means Claude's chain gate
or GPU byte-identity subset, and the skeleton timing gates where marked. Agents do not run GPU
tests.

| Step | Slot / change | Legacy files to move | Blocked by (2026-10-24) | Depends on | Risk | Est. h | Verify |
|---|---|---|---|---|---|---|---|
| 0 | **Freeze + include sweep** (not a module move) | none. Switch the forwarder includers to the `<VP-V/...>` paths: `LibplaceboVideoRenderer.cpp`/`.h`, `IRenderer.h`, `MainConfigSchema.h`, `ConfigEditorWindow.cpp`, `RendererCaptureHarnessTests.cpp`, `SourceRebase.cpp`, `SourceResolutionMonitor.cpp`. Then delete all 15 forwarding headers and their `forwarding_headers` entries | all of them (active branches) | every feature branch merged at M1 | low | 1.0 | build, CPU |
| 1 | `drive.led_measure` (rest) | `led/LedMeasurementHook.*` | test project compiles the `.cpp` | 0 | low | 0.5 | build |
| 2 | `drive.led_compensation` (rest) | `led/LedRawTransfer.*`. Decide where `led/LedDriveConfig.h` belongs (the drive's config schema): here or `control.config` (step 19) | test project; `LedDriveConfig.h` included by the Config GUI | 0 | low | 0.75 | build |
| 3 | `output.calibration_lut` (rest) | `vprenderer/LibplaceboDisplayLut.*`, `vprenderer/LibplaceboCalibrationLutPolicy.h` | test project; renderer includers | 0 | low | 0.75 | build |
| 4 | `colour.extended_range` | `vprenderer/ExtendedRange.*` | test project | 0 | low-medium (BTB/WTW path) | 0.75 | build, CPU, GPU (BTB/WTW probes) |
| 5 | `output.dither` | `vprenderer/SigmaDeltaDomain.h`, `OutputEncodingRepair.*`, `CustomHookChain.*` | test project | 0 | medium (output hook chain, libplacebo PRE_OUTPUT repair) | 1.5 | build, CPU, GPU |
| 6 | `colour.sdr_tone_shape` | `vprenderer/SdrToneShape.h`, `vprenderer/DolbyVisionTrims.h`. The shaders stay in `shaders/` (runtime layout) | dv-l8 changes this module | dv-l8 merged, 0 | medium (DV trims) | 1.0 | build, CPU, GPU (DV corpus) |
| 7 | `colour.tone_gamut` (rest) | `vprenderer/LibplaceboRenderParameters.cpp`, `ToneMappingTuning.h` (included by `RendererProfileConfig.h` and the Config tone-mapping control) | test project; `RendererProfileConfig.h` (dv-l8) | 0 | low-medium | 1.0 | build |
| 8 | `scaling.output` | `vprenderer/AdaptiveDownscale.*`, `vprenderer/DownscaleLight.h` | test project | 0 | low-medium | 0.75 | build, CPU |
| 9 | `scaling.geometry` (rest) | `vprenderer/NlsMaskMapping.h`, `NlsHookRecovery.h`, `AnamorphicPresentation.h`, `AlphaSourceCropPolicy.*` (about 20 test includers; crop pipeline) | crop-pipeline includers; renderer | 0 | medium | 1.5 | build, CPU, GPU |
| 10 | `input.source_analysis` (rest) | `SceneDetector.*`, `ActivePicture*` (analyzer, decision timeline, evidence, transition model), `AnalysisFrame.*`, `SourceResolutionDetector.*`, `vprenderer/SourceResolutionMonitor.*` | not attempted early (cross-cutting; srcres latch) | 0 | medium (many includers, capture pins) | 2.0 | build, CPU |
| 11 | `repair.deband` | `vprenderer/VpDebander.*` | entrance stage (B3); test project | B3 merged, 0 | medium | 0.75 | build, CPU, GPU (entrance byte identity) |
| 12 | `repair.texture_restore` | `vprenderer/TextureRestore.*`, `vprenderer/DetailPulse.*` | entrance stage (B3); test project | B3 merged, 0 | medium | 1.0 | build, CPU, GPU |
| 13 | `scaling.rebase` (rest) | `vprenderer/SourceRebase.*` | entrance stage (B3) | B3 merged, 0 | medium | 0.75 | build, CPU, GPU |
| 14 | `input.signal` | `HdrInference.*`, `HDRData.cpp`, `ColorSpace.cpp`, `EOTF.cpp`, `DolbyVisionTunnelGate.h`, `DolbyVisionTunnelHandoff.h`, `vprenderer/GpuDolbyVisionTunnelUnpack.*`, `vprenderer/Bt2020SignalController.h` | DV code; dv-l8 | dv-l8 merged, 0 | medium-high (DV tunnel) | 2.0 | build, CPU, GPU (DV autotest) |
| 15 | `input.capture` | `vprenderer/GpuCaptureUnpack.*`, `GpuRgbUnpack.*`, `AlphaNativeRgbIngress.h`, `UploadReuse.h`, `EarlyUpload.*`, `CaptureInput.*`, `VideoConversionOverride.cpp`, `video_frame_formatter/`, `blackmagic_decklink/`, `magewell/` (sub-folders, like `calibration_lut/calibration/`) | test project; dv-l8 (formatter) | 14 | high (SDK include paths, `MagewellSdkRoot`, `VpCiNoMagewell`, packaging checks) | 3.0 | build (with and without Magewell SDK), CPU, GPU |
| 16 | `motion.estimation` | `vprenderer/NvOpticalFlow.*`, `vprenderer/MotionAheadProducer.cpp`, `MotionAhead.h`, `VectorStore.h`, `MotionValidity.h`, `OpticalFlowTrust.h`, `MotionConfig.h`, `vprenderer/MotionEngineIdle.h` | de-judder/B3; test project | B3 merged, 0 | high | 2.0 | build, CPU, GPU |
| 17 | `motion.vector_delivery` + `motion.consumers` (two commits) | `vprenderer/DejudderPolicy.h`, `DejudderWarp.h`, then `DejudderWarp.cpp` (also listed by estimation; it belongs to consumers), `DejudderAhead.h`, `RenderedWarp.cpp`, `FrameInterpolator.cpp`, `InterpolationTimeline.h`, `InterpolationPolicy.h`, `OcclusionMaskHook.cpp`, `TemporalRenderer.h` | de-judder/B3 (`FrameInterpolator.cpp` in the skeleton worktree); test project | 16 | high | 3.0 | build, CPU, GPU + skeleton timing gates |
| 18 | `input.queue_timing` + `output.presentation` (two commits) | `QueueLatency.h`, `QueueConfiguration.h`, `QueueLookahead.h`, `CaptureFrameQueue.h`, `VideoTimingController.cpp`, `TimingClock.cpp`, `UnifiedProfileRuntime.cpp`, `vprenderer/PulldownDetector.h`, `vprenderer/AlphaQueuePolicy.h`; then `vprenderer/RenderAheadPolicy.h`, `AlphaPresentationTelemetry.cpp`, `OpenLoopDisplayRate.h`, `DisplayRefreshRatePolicy.cpp`, `NativeStatsOverlayBitmap.h` | timing/queue (B3), render-ahead | 16-17 | high (timing: CPU tests are not enough) | 3.0 | build, CPU, GPU + skeleton timing gates + render-ahead check on the FL35 |
| 19 | `control.config` | `ConfigFile.cpp`, `ConfigSchema.h`, `MainConfigSchema.h`, `RendererProfileConfig.h` (schema only; the Config app `src/VideoProcessor-Config` stays a separate app; `led/LedDriveConfig.h` if step 2 sends it here) | config/settings work (settings-preview, dv-l8) | settings-preview merged, all above | high (nearly every project includes the schema; Config tests) | 3.0 | build, CPU, Config suite (`run_config_tests.ps1`, off screen) |

**Total:** about 30 agent hours of moves and checks. Allow about 35 h including bisects and the GPU
checkpoints, which are Claude's and depend on GPU availability.

### Not moved (by design)

- `src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp`/`.h`: Bill's renderer host stays
  in place and calls into the modules (ADR-0022). Slicing per-slot code out of it is separate
  feature work, not part of M2.
- `shaders/*.glsl` (`LED compensation`, `CfL_Prediction`, NLS, SSIM, sigma-delta, DV trims, SDR tone
  shape): the runtime shader folder is part of the deploy layout. The manifests keep pointing at it.
- `src/VideoProcessor-Lib/DebugLog.h` and `src/VideoProcessor-GUI/StatsOverlayWindow.cpp`
  (`control.diagnostics`): shared logging infrastructure and GUI code. If logging needs a VP-V
  home, use `src/VP-V/core/`, not a module.
- `src/VideoProcessor-GUI/VideoProcessorDlg.cpp` (listed by `input.queue_timing`): the main GUI
  dialog stays in the GUI project.

## Forwarding headers: when each one goes

All 15 are removed in **step 0**: their includers are switched to the new paths in the same commit.
Fallback: if step 0 has to be split, remove each one with its includer.

| Forwarding header (legacy path) | Module | Includers that keep it alive | Removed |
|---|---|---|---|
| `src/VideoProcessor-Lib/KernelInverse.h` | `scaling.rebase` | `LibplaceboVideoRenderer.cpp`, `SourceRebase.cpp`, `SourceResolutionMonitor.cpp` | step 0 (fallback: step 13) |
| `src/VideoProcessor-Lib/SubtitleBoxDetector.h` | `input.source_analysis` | `LibplaceboVideoRenderer.cpp` | step 0 |
| `src/VideoProcessor-Lib/SubtitleTemporalFill.h` | `input.source_analysis` | `LibplaceboVideoRenderer.cpp` | step 0 |
| `src/VideoProcessor-Lib/SubtitleBoxLookahead.h` | `input.source_analysis` | `LibplaceboVideoRenderer.h` | step 0 |
| `src/VideoProcessor-Lib/led/LedDriveModule.h` | `drive.led_compensation` | `LibplaceboVideoRenderer.cpp` | step 0 |
| `src/VideoProcessor-Lib/led/LedDriveArbiter.h` | `drive.led_compensation` | `LibplaceboVideoRenderer.cpp` | step 0 |
| `src/VideoProcessor-Lib/led/LedDriveHoldover.h` | `drive.led_compensation` | `LibplaceboVideoRenderer.cpp` | step 0 |
| `src/VideoProcessor-Lib/led/SerialPort.h` | `drive.projector_control` | `ConfigEditorWindow.cpp` | step 0 (fallback: step 19) |
| `src/VideoProcessor-Lib/led/ConnectionTest.h` | `drive.projector_control` | `ConfigEditorWindow.cpp` | step 0 (fallback: step 19) |
| `src/VideoProcessor-Lib/vprenderer/LibplaceboOutputPolicy.h` | `colour.tone_gamut` | `LibplaceboVideoRenderer.cpp` | step 0 |
| `src/VideoProcessor-Lib/calibration/CalibrationPattern.h` | `output.calibration_lut` | `LibplaceboVideoRenderer.cpp`, `IRenderer.h` | step 0 |
| `src/VideoProcessor-Lib/calibration/CalibrationController.h` | `output.calibration_lut` | `LibplaceboVideoRenderer.cpp`, `RendererCaptureHarnessTests.cpp` | step 0 |
| `src/VideoProcessor-Lib/calibration/CalibrationSimulation.h` | `output.calibration_lut` | `LibplaceboVideoRenderer.cpp`, `RendererCaptureHarnessTests.cpp` | step 0 |
| `src/VideoProcessor-Lib/calibration/PatternCodes.h` | `output.calibration_lut` | `LibplaceboVideoRenderer.cpp`, `RendererCaptureHarnessTests.cpp` | step 0 |
| `src/VideoProcessor-Lib/calibration/CalibrationConfig.h` | `output.calibration_lut` | `MainConfigSchema.h` | step 0 (fallback: step 19) |

Before step 0, run `git grep` for each forwarder's old include spelling. The list above is from
2026-10-24; branches merged at M1 may have added includers. After step 0, the drift check still
passes because nothing lists forwarders. `src/VideoProcessor-Lib/calibration/` is then empty and
disappears.

## Verification plan for the M2 window

- After every commit: x64 Release build (delete stale `.iobj`/`.ipdb` first) and
  `python tools/check_module_manifests.py`.
- Full CPU suite at steps 0, 5, 10, 13, 15 and 19, plus after 17 and 18. Bisect a failing range.
- GPU checkpoints (Claude): output side after step 6; entrance after step 13 (entrance byte
  identity, the B3 path); capture/DV after step 15; motion and timing after steps 17 and 18
  (skeleton timing gates, render-ahead). Use the chain gate (Beta 377 first) before any deploy
  built from M2.
- Update `migration-path-map.md` (`tools/migration_path_map.py --write`) in each commit. The check
  fails otherwise. It also flags living docs that still name an old path; fix them with `--fix`.
