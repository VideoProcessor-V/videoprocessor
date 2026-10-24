# Early migration inventory (2026-10-24)

Early, opportunistic moves of legacy code into `src/VP-V/modules/<unit>/<slot>/` (ADR-0017,
ADR-0022), limited to code that **no active branch is touching**, so the moves cannot collide with
ongoing work. Branch: `chore/early-migrations`, from `feat/skeleton` at `b8b87db0`.

## How "safe" was decided

The change list of every active branch was taken against `feat/skeleton`
(`git diff --name-only <merge-base>..<branch>`), plus the uncommitted work in each active worktree
(`git status --porcelain`, read-only):

| Branch / worktree | Files it changes (summary) |
|---|---|
| `feat/skeleton` (uncommitted, `D:\codex\videoprocessor\skeleton`) | `LibplaceboVideoRenderer.cpp`, `FrameInterpolator.cpp`, `AlphaQueuePolicy.h`, `RendererCaptureHarnessTests.cpp`, `RealClipLibrary.h`, `PipelineSkeletonTests.cpp`, `VP-pipeline-final.md`, ADR-0024 |
| `feat/net-runner` (+ worktree `nets`) | `src/VP-V/nets/**`, `control/net_runtime` manifest + README, `src/VP-V/README.md`, `components-master.json`, **`VideoProcessor-Test.vcxproj(.filters)`**, `tools/net_stamp.py` |
| `fix/vpscalerender-motion` | `LibplaceboVideoRenderer.cpp`, **`VideoProcessor-Test.vcxproj`**, `VpScaleRenderMotionTests.cpp`, `tools/vp-scale-render/**` |
| `feat/dv-l8` (worktree `dv-l8`, uncommitted) | DV formatter + DV trims shader, `RendererProfileConfig.h`, `ConfigEditorWindow.cpp`, `ConfigEditorWindowTests.cpp`, `LibplaceboVideoRenderer.cpp`, `colour/sdr_tone_shape` + `input/signal` modules, `components-master.json`, **`VideoProcessor-Test.vcxproj(.filters)`** |
| `feat/settings-preview` (+ worktree) | `src/VP-V/config/**`, `ConfigEditorWindow.cpp`, `AppMain.cpp`, `ConfigEditorWindowTests.cpp`, settings docs/tools, `run_config_tests.ps1` |
| `fix/r6-baseline` | nothing beyond `feat/skeleton` (already merged) |

Also avoided by instruction: `LibplaceboVideoRenderer.cpp`, the entrance / de-judder / B3 / timing /
queue code, the Config GUI and settings code, DV code, `RendererCaptureHarnessTests.cpp`,
`RealClipLibrary.h`.

A module is **SAFE** when none of its files is on those lists and every project file that compiles
it is on none of them. Includers on the lists are not edited: the moved header leaves a
**forwarding header** at its legacy path instead (below). **BLOCKED** names the reason.

The decisive constraint is `src/VideoProcessor-Test/VideoProcessor-Test.vcxproj.vcxproj`: three
active branches change it, so any `.cpp` that the test project compiles directly (most renderer
`vprenderer/*.cpp` units) cannot move yet without editing it.

## Forwarding headers (temporary, removed in M2)

A forwarder is a legacy-path header containing only `#pragma once` and
`#include <VP-V/modules/<unit>/<slot>/<Name>.h>`, with the marker comment
`VP-V forwarding header`. Each is listed in its module's `MANIFEST.json` under
`"forwarding_headers"`; `tools/check_module_manifests.py` accepts exactly those (marker, listed, pure
forward to the moved file of the same name) and still fails on any other duplicate name. New code
includes the `<VP-V/...>` path. Remove the forwarders in M2, after the listed includers
(`LibplaceboVideoRenderer.cpp`/`.h`, `ConfigEditorWindow.cpp`, `RendererCaptureHarnessTests.cpp`,
`IRenderer.h`, `MainConfigSchema.h`, `SourceRebase.cpp`, `SourceResolutionMonitor.cpp`) switch to the
new path. To find them all: `forwarding_headers` in the manifests.

## Inventory

| Slot | Candidate files | Verdict | Why / notes |
|---|---|---|---|
| `control.diagnostics` | `CrashState.*` (crash state ring), `StateReport.*`, `StateReportSettings.h` | **SAFE** | Lib-only; includers GUI app/dialog + tests (on no list). `StatsOverlayWindow.cpp` (GUI) and `DebugLog.h` (used everywhere) stay |
| `drive.projector_control` | `led/LedController.*`, `BarcoAsciiDriver.*`, `ProjectorDriver.*`, `SerialPort.*` (RS-232), `ConnectionTest.*`, `LedClock.h`, `LedShutdownRestore.h` | **SAFE** | Lib + the Config / ConfigTests standalone projects (their `.vcxproj` on no list). Forwarders: `led/SerialPort.h`, `led/ConnectionTest.h` (included by `ConfigEditorWindow.cpp`). `led/LedDriveConfig.h` stays (included by Config GUI + `ConfigEditorWindowTests.cpp`) |
| `drive.led_measure` | `led/LedMeasurement.*`, `led/LedLookahead.*` | **SAFE (partial)** | `LedMeasurementHook.*` stays: its `.cpp` is compiled by `VideoProcessor-Test.vcxproj` |
| `drive.led_compensation` (adaptive gamma) | `led/LedDriveModule.*`, `LedDriveArbiter.*`, `LedDriveHoldover.*` | **SAFE (partial)** | Forwarders for all three headers (renderer includes them). `LedRawTransfer.*` stays (test project compiles it); `shaders/LED compensation.glsl` stays (runtime shader folder) |
| `scaling.rebase` | `KernelInverse.*` (kernel inverse helper) | **SAFE (partial)** | Forwarder `KernelInverse.h` (renderer; the entrance and source-resolution includers `SourceRebase.cpp` / `SourceResolutionMonitor.cpp` are left untouched and use it too). `SourceRebase.*` is entrance code (B3): BLOCKED. Also listed by `input.source_analysis`; both manifests updated |
| `input.source_analysis` | subtitle detection: `SubtitleBoxDetector.*`, `SubtitleBarEvidence.h`, `SubtitleBoxLookahead.h`, `SubtitleOpaqueCardRefinement.h`, `SubtitleTemporalFill.h` | **SAFE (partial)** | Forwarders `SubtitleBoxDetector.h`, `SubtitleTemporalFill.h` (renderer `.cpp`), `SubtitleBoxLookahead.h` (`LibplaceboVideoRenderer.h`). Placement/renderer-side subtitle headers stay; `SubtitleDetectionOptimization.h` / `SubtitleGeneratedGrayStyle.h` stay (included by `RendererProfileConfig.h`, dv-l8) |
| `input.source_analysis` (rest) | `SceneDetector`, `ActivePicture*`, `AnalysisFrame`, `SourceResolution*` | not this round | Cross-cutting crop analysis with many includers; `SourceResolution*` is the srcres latch work. Candidate for the M1-M2 phase |
| `scaling.geometry` | `NlsGeometryPolicy.*` | **SAFE (partial)** | Lib-only; includers `MadVRShaderRuntimeState.h` + tests. `Nls*.h`, `AnamorphicPresentation.h`, `AlphaSourceCropPolicy.*` stay (renderer / crop pipeline includers) |
| `colour.tone_gamut` | `LibplaceboOutputPolicy.*` | **SAFE (partial)** | Lib-only; forwarder for the header (renderer). `LibplaceboRenderParameters.cpp` is compiled by the test project: BLOCKED; `ToneMappingTuning.h` is included by `RendererProfileConfig.h`: BLOCKED |
| `output.calibration_lut` | `calibration/` (45 files: meter, ArgyllCMS, session, patterns) | **SAFE (partial)** | Lib-only; moved as a sub-folder `calibration_lut/calibration/`. Forwarders for `CalibrationPattern.h`, `CalibrationController.h`, `CalibrationSimulation.h`, `PatternCodes.h` (renderer, `RendererCaptureHarnessTests.cpp`, `IRenderer.h`) and `CalibrationConfig.h` (`MainConfigSchema.h`, config code, left untouched). `LibplaceboDisplayLut.cpp` is compiled by the test project: BLOCKED |
| `input.capture` | capture unpack, early upload, DeckLink/Magewell, formatters | BLOCKED | Renderer `.cpp`s compiled by the test project; `video_frame_formatter` changed by dv-l8 |
| `input.signal` | HDR inference, DV gate/handoff, DV unpack | BLOCKED | DV code; dv-l8 changes this module |
| `input.queue_timing` | queue, timing clock, `PulldownDetector.h` (cadence/pulldown) | BLOCKED | Timing/queue code (B3); `AlphaQueuePolicy.h` in the skeleton worktree |
| `motion.estimation` / `vector_delivery` / `consumers` | NVOF, de-judder, interpolator, warps | BLOCKED | De-judder/B3 code; `FrameInterpolator.cpp` in the skeleton worktree |
| `repair.deband`, `repair.texture_restore` | `VpDebander`, `TextureRestore`, `DetailPulse` | BLOCKED | Entrance stages (B3); test project compiles them |
| `scaling.output` | `AdaptiveDownscale`, `DownscaleLight.h` | BLOCKED | Test project compiles `AdaptiveDownscale.cpp` |
| `colour.extended_range` | `ExtendedRange.*` | BLOCKED | Test project compiles `ExtendedRange.cpp` |
| `colour.sdr_tone_shape` | `SdrToneShape.h`, `DolbyVisionTrims.h`, shaders | BLOCKED | dv-l8 changes this module and the DV trims shader |
| `output.dither` | `SigmaDeltaDomain.h`, `OutputEncodingRepair`, `CustomHookChain` | BLOCKED | Test project compiles the `.cpp`s |
| `output.presentation` | render-ahead, telemetry, refresh policy | BLOCKED | Timing code (render-ahead) |
| `control.config` | config schema, `RendererProfileConfig.h`, Config GUI | BLOCKED | Config/settings code; settings-preview and dv-l8 change it |
| `scaling.chroma_upscale` | `shaders/CfL_Prediction.glsl` | not moved | Runtime shader folder (deploy layout), not source |
| `colour.appearance` (Stage 13 CAM16), `drive.black_level`, `repair.encoding`, `scaling.route_4k`, `scaling.refine`, `control.net_runtime` | none | n/a | Placeholders: no legacy code to move |

## Old -> new paths

[`migration-path-map.md`](migration-path-map.md) lists every moved file (generated from git renames
and the forwarding headers by `python tools/migration_path_map.py --write`). Living documents,
scripts and source comments were updated to the new paths; dated historical records
(`docs/agent-reports/`, `docs/audit/`, prompts) keep the old ones and resolve through the map.
`tools/check_module_manifests.py` fails if the map is stale or a living file names an old path.

## Results

Filled in as each module lands (one module per commit, x64 Release build + CPU suite green after
each). See the merge note below.
