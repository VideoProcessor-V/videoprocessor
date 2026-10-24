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
| `input.source_analysis` | subtitle detection: `SubtitleBoxDetector.*`, `SubtitleBarEvidence.h`, `SubtitleBoxLookahead.h`, `SubtitleOpaqueCardRefinement.h`, `SubtitleTemporalFill.h` | **BILL-OWNED (not moved)** | All are in Bill's upstream `v1.4.02-beta`. Bill's code stays where he has it (owner, 2026-10-24); the move was made, then dropped. VP-side subtitle features are extracted into VP-V adapters instead (M2 plan, option (d)) |
| `input.source_analysis` (rest) | `SceneDetector`, `ActivePicture*`, `AnalysisFrame`, `SourceResolution*` | not this round | Cross-cutting crop analysis with many includers; `SourceResolution*` is the srcres latch work. Candidate for the M1-M2 phase |
| `scaling.geometry` | `NlsGeometryPolicy.*` | **BILL-OWNED (not moved)** | In Bill's upstream; move dropped. VP's NLS additions (`NlsMaskMapping.h`) stay VP's |
| `colour.tone_gamut` | `LibplaceboOutputPolicy.*` | **BILL-OWNED (not moved)** | In Bill's upstream; move dropped. Its dead strict Full-G22 code is recorded as a note for Bill (findings item 6), not changed |
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

The BLOCKED modules and the forwarder removal are planned in [`migration-m2-plan.md`](migration-m2-plan.md).

## Results

Branch `chore/early-migrations` was rebased onto `feat/skeleton` `fd60b650` (B3) on 2026-10-24.

| # | Commit | Module | Forwarders |
|---|---|---|---|
| 1 | `259cfa50` | `control.diagnostics`: CrashState, StateReport, StateReportSettings | 0 |
| 2 | `570b1ee8` | `drive.projector_control`: LED controller, Barco/projector drivers, RS-232, connection test, clock, shutdown restore | 2 |
| 3 | `9bfb55d6` | `drive.led_measure`: LedMeasurement, LedLookahead | 0 |
| 4 | `9e055034` | `drive.led_compensation`: LedDriveModule, arbiter, hold-over | 3 |
| 5 | `648da813` | `scaling.rebase`: KernelInverse | 1 |
| 6 | `d2517eb1` | `output.calibration_lut`: `calibration/` (45 files) | 5 |

Dropped on 2026-10-24 (owner principle: Bill's code stays where he has it): the moves of subtitle
detection, NlsGeometryPolicy and LibplaceboOutputPolicy, all in Bill's upstream, and the two fixes
that edited Bill's code (subtitle temporal-fill margins; the output policy's dead strict Full-G22
code). Both fixes are now notes for Bill in the findings doc. `tools/migration_path_map.py` now fails
the drift check if a file in Bill's upstream beta is moved or owned by a module. The pre-drop
branch is kept locally as `backup/early-migrations-pre-bill-drop`.
Verification plan (coordinator, 2026-10-24): an x64 Release build at every migration commit; the
full CPU suite (single-threaded, TRX) at commit 5 and at commit 6 (the last move); one subtitle probe tool build;
then the `fix:` commits (see `module-findings-2026-10-24.md`) built and tested at the branch tip.
The status of each run is in the hand-off report. A commit is marked verified here only after its
run is green.

## Merge note

**Order.** Merge after B3 (`feat/skeleton` at or after `fd60b650`, already the base) and after any
branch that adds includes of the moved headers. None of the active branches does: checked against
feat/skeleton, feat/net-runner, fix/vpscalerender-motion, feat/dv-l8, feat/settings-preview and
fix/r6-baseline. Merge the migration commits (1-6 plus the docs commits) as one unit. The `fix:`
commits after them can merge with them or separately, but never before them.

**Expected conflicts: none textual.** `git merge-tree` against every active branch tip is clean.
Generated files and checks to re-run after each merge:

1. `docs/design/components-master.json` is generated. Several branches touch it, so after each merge
   run `python tools/check_module_manifests.py --write` and commit the result if it changed.
2. `docs/design/migration-path-map.md` is generated from git. Run
   `python tools/migration_path_map.py --write` if the check reports it stale. It will not, unless
   another move lands.
3. The README settings blocks: run `python -I tools/vpv_tests/seed_module_readmes.py` after a branch
   that changes `slots.json`, because this branch changed the generator's link depth.
4. A branch that adds a new `#include` of a moved header by its old path still builds while that
   header has a forwarder (11 of them). For the headers without a forwarder (CrashState,
   StateReport, LedController, ProjectorDriver, BarcoAsciiDriver, LedClock, LedShutdownRestore,
   LedMeasurement, LedLookahead and most of
   `calibration/`), it fails to compile. Switch such an include to the `<VP-V/...>` path given in
   the path map.
5. After the merge, build the solution and run the CPU suite once on the merged tip.

The forwarding headers go in M2 step 0 (`migration-m2-plan.md`).
