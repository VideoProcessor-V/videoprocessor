<!-- Copyright(C) 2026 VP-V contributors; SPDX-License-Identifier: GPL-3.0-only -->

# Hook contract of `output.calibration_lut`

This is the list of every place where the calibration module and the rest of VP touch each other, today and
as it is meant to be after the M2 extraction (waves 2 and 3 of reset 1). Line numbers are for
`src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp` (written `LVR`) at integration base `bcde6fc2`;
they drift, so search for the named symbol.

How to read the "target hook" column: names marked **(design)** are in the M2 designs (doc 52 for the renderer
segments, doc 60 for the render-loop and Config hooks, doc 63 for the minimal call sites). Names marked
**(proposed)** are this document's suggestion; they are not agreed, and the hook owner of that wave decides.
Status after W1-CAL-B (renderer side): rows marked **done** have moved; the "today" column keeps the old call site for reference. Unmarked rows were already a single adapter call and stay as they are.

Rules that hold for every hook (module README, ADR-0013, owner rules):
- Bill's files get generic, inert call sites only. With the module absent or off, behaviour is byte-identical.
- A failure is never silent: each automatic downgrade engages a "Fallbacks engaged" entry (the ids are in
  `FallbackCatalog`; listed under "Failure and fallback" below).
- No frame rate, refresh rate or resolution is hard-coded; the patch geometry and the output format come from
  the renderer's facts (`PatternOutputInfo`).

## 1. Renderer side (the render thread owns the GPU)

Threading summary: everything in the renderer table runs on the **render thread** under `renderMutex`, except
the rows marked "RPC thread", which come from the calibration worker or the Config RPC thread and only touch
the adapter's mutex-guarded or atomic state (never the GPU).

### 1a. Patch display (calibration patterns replace video)

| # | Today (call site) | Target hook | Inputs | Outputs | Thread / locks |
|---|---|---|---|---|---|
| P1 | `LibplaceboVideoRenderer::SetCalibrationPattern` (LVR 32896): stores the request in `calibrationHost.SetPattern`, sets `m_calibrationPatternActive`, suspends Dynamic Drive (`m_dynamicDrive.SuspendForCalibration`), `BeginQueueGeneration`, notifies `m_queueChanged`; on end `EndPattern`, `ResumeAfterCalibration`, `ReleaseFallback("calibration patch")` | **(design)** the IVideoRenderer API stays (plugin API 39, `IRenderer.h` 755); its body becomes `module.SetPattern(request) -> reason` plus two generic lifecycle calls (`OnPatternsBegin` / `OnPatternsEnd`, **proposed**) for the Dynamic Drive suspend and the queue generation | `PatternRequest` (POD, `pattern_lib/CalibrationPattern.h`): `active`, `generation`, `patch[3]`, `background[3]`, `windowAreaPercent`, `flags` | `bool` + reason text | RPC thread (calibration worker). Takes no GPU lock; the adapter's `patternMutex` guards the stored request |
| P2 | Render loop, `RenderLoop` (LVR 35542): when `m_calibrationPatternActive`, takes `renderMutex`, calls `Impl::PresentCalibrationPattern` (LVR 9862 -> `Adapter::Present`) instead of dequeuing video, then `continue` | **(design)** R33 `TakeOverPresentation() -> bool presented` (doc 60, hook H1, default false, "P11"): called in the render loop before dequeue; takes `renderMutex` itself | none from the loop; the module reads its own pattern state. The renderer gives the surface: `Surface{ gpu, vpOwnedSwapchain, swapchain, negotiatedSwapchainFormat, ReadR10Texture }` | `PresentResult{ presented, patchWindow }`; `presented == false` makes the loop wait 20 ms | Render thread, `renderMutex` |
| P3 | `WaitForDequeueLocked(lock, wakeOnCalibrationPattern)` (LVR 31036) wakes when a pattern starts; `OnVideoFrame` returns early while patterns are active (LVR 30243: frames are not queued) | **(proposed)** the same R33 predicate (`IsTakingOver()`) read by both places | none | `bool` | Render thread (wake), capture thread (frame drop): atomic read only, no lock |
| P4 | `Impl::ReleaseGpu` (LVR 9783) calls `calibrationHost.ReleaseGpu(gpu)` | **(design)** the generic GPU-resource release call of the output segments (`OnGpuRelease`, **proposed** name) | `pl_gpu` | the patch texture and the VP-stage LUT texture freed | Render thread |
| P5 | `GetCalibrationOutput` (LVR 32860): try-locks `renderMutex` (one try while patterns show), publishes `OutputFacts` (`PublishOutputFacts`), returns `FillOutputInfo` | **(design)** stays an `IVideoRenderer` method; the facts are produced by a generic `GetOutputFacts()` renderer view (**proposed**) | none | `PatternOutputInfo` (format, range, transfer, refresh, presented generation, read-back, HDR tone curve) | Called from the RPC/worker thread; reads atomics and a cached copy |
| P6 | Output-format notes: `NoteVideoTarget` (LVR 22956), `NoteToneCurve` (LVR 27790, PQ output only) | **(proposed)** `OnFrameTarget(target, source)` (read-only observer inside the colour stage collection of doc 52 item f) | the frame's target transfer; source and target `pl_color_space`, libplacebo colour-map parameters | the cached tone curve for `PatternOutputInfo` | Render thread |

Status after W1-CAL-B (renderer side):

- P1: **Done W1-CAL-B:** `Adapter::SetPatterns(request, patternsActive, PatternLifecycle)`; the renderer keeps its state checks and passes five lambdas (drive suspend/resume, queue generation, notify, fallback release).
- P2: **Done W1-CAL-B:** `CalibrationLutHost::TakeOverPresentation(patternsActive, TakeOverEnv)` returns `NotTaking/Stop/Handled`; stop flag checked first under the queue lock, then `renderMutex` for the present, 20 ms wait when not presented (as before).
- P3: **Done W1-CAL-B:** all three reads use `CalibrationLutHost::IsTakingOver(flag)`. The flag stays a renderer atomic (header unchanged, no m_impl lifetime dependency on the capture thread).
- P4: Already one adapter call (`ReleaseGpu`); unchanged.
- P5: **Done W1-CAL-B:** `Adapter::CollectOutputInfo(renderMutex, patternsActive, gather, info)` holds the try-lock loop; the renderer only supplies `gather` (runs under `renderMutex`).
- P6: **Open:** `NoteToneCurve` (LVR ~27790) is in zone Z2 (held by another team) and `NoteVideoTarget` (~22956) is a one-line adapter call; both unchanged.


Fidelity contract of a patch (do not break when moving the call site): the module writes **raw integer codes**
into the swapchain backbuffer in its format. No libplacebo pass, tone/gamut mapping, LUT, deband, dither or
sigma-delta touches it; swapchain format, range, DXGI colour space and HDR signalling stay as VP set them for
video. BTB and WTW codes reach the display unchanged. A windowed patch that the surface cannot take is shown full
field and engages the `calibration patch` fallback (LCD and OLED readings depend on the window).

### 1b. LUT apply (the projector LUT)

| # | Today (call site) | Target hook | Inputs | Outputs | Thread / locks |
|---|---|---|---|---|---|
| L1 | `LoadDisplayLut` (LVR 12565..12608): `LibplaceboCalibrationLut::Select` picks the slot, `LibplaceboDisplayLut::Load` parses the `.cube`, then `calibrationHost.NoteLutFileVersion` | stays Bill's loader (never moves); **(proposed)** the module only gets `OnLutLoaded(fileVersion, tick)` | LUT path, parse result, file version | `Reload()` state for the reload policy | Render thread |
| L2 | `MaybeReloadDisplayLut` (LVR 12657): `calibrationHost.LutReloadDue(path, enabled, tick)` once per frame | **(proposed)** `ReloadDue(...) -> bool` in the module's BeforeRender hook; `calibration_lut_reload` = `auto` / `apply` (planned key) | path, enabled, `GetTickCount64()` | bool (auto mode probes the file once a second) | Render thread |
| L3 | `ConfigureDisplayLutForTarget` (LVR 12700): `calibrationHost.TakeLut(lut, {stage == "vp", originalPathway, slotRuns}, path, status)`; returns true when the VP stage owns the LUT (then `target.lut` stays null) | **(design)** `BeforeRender` of the core segments extension (doc 52 item f, "S2 colour") | parsed `pl_custom_lut`, `StageChoice{ vpStage, original, slotRuns }`, path | whether libplacebo's target LUT is cleared; the stage's LUT set; status text (written only on a change) | Render thread |
| L4 | Hook assembly (LVR 27833): `colourFrame.calibrationLut = calibrationHost.HookForFrame() != nullptr`; S2 pushes `Adapter::Stage()` as the last colour hook (slot id `output.calibration_lut`, order 250, after LED compensation and black level, before the de-judder picture cache) | **(design)** `CoreSegmentsExtension::CollectHooks` (R7, doc 52 item f); the module returns its `pl_hook` and order key | none (the hook samples the PRE_OUTPUT texture) | `pl_hook*` or null; same-size texture out | Render thread; hook runs inside libplacebo's render call |
| L5 | `AfterRender` (LVR 28397): `calibrationHost.AfterRender(hookErrors)`; `LutDropped` (LVR 28468); `LutAttached` (LVR 28473) | **(design)** `AfterRender` (doc 52 item h, "calibration revert") | libplacebo's disabled-hook list; render result; whether the target LUT was applied | `FallbackCatalog::Decision`; the "attached" flag read by status and Config | Render thread |
| L6 | Repeat-render reuse: `known[]` (LVR 7755) lists `calibrationHost.Stage().Get()` as a `Generation` hook; `DisplayLutSignature` (LVR 7780) | **(proposed)** the module declares its repeat-reuse kind and signature through its hook descriptor | the stage's LUT generation; observed file version | `uint64_t` signature | Render thread |
| L7 | Impl members: `CalibrationLutHost::Adapter calibrationHost` (LVR 8692); `outputColour{ ..., calibrationHost.Stage() }` (LVR 8732) | **(design)** `core/CoreSegmentsExtension` owns the member (doc 52 item c) | - | - | - |
| L8 | Status: `status.calibrationLutAttached` (LVR 32652), Ctrl+I `LUT:` line, `ActiveProfileStatus.calibration*` | **(proposed)** `StatusLines()` provider | attached flag, config identity and path | text, flags | UI/OSD threads read a copy |

`CalibrationLutStage::Hook::Apply` (the GPU work): samples the PRE_OUTPUT texture (nearest), keeps the part
outside 0..1 in a shader global, clamps, runs `pl_shader_custom_lut`, adds the excess back through the LUT's edge
slope (BTB/WTW, ADR-0002), returns a new texture of the same size. `Release` frees the LUT shader object.

## 2. Process and Config side (calibration wizard)

| # | Today (call site) | Target hook | Inputs | Outputs | Thread / locks |
|---|---|---|---|---|---|
| C1 | GUI: `CVideoProcessorDlg::HandleCalibrationRpc` (`VideoProcessorDlg.cpp` 3286) builds `CalibrationController::Hooks{ renderer, applied, clock, ... }` (3298..3333); `WM_MESSAGE_CALIBRATION_APPLIED` (`VideoProcessorDlg.h` 110, handler `OnCalibrationApplied`) re-applies the saved configuration; `m_videoRenderer->RequestCalibrationLutReload()` (3639) | **(design)** H4 lifecycle hooks (doc 60 R27, R29): `OnRendererCreated/Retired`, `OnShutdown`; the RPC handler is dispatched by operation id | command text + arguments | status text | UI thread builds; the controller answers at once, its worker runs the session |
| C2 | RPC: `ConfigurationRpcProtocol::Operation::Calibration = 11` (`ConfigurationRpcProtocol.h` 47), `ConfigurationRpcClient::Calibration(command, arguments, ...)` | stays (wire protocol) | `status`, `detect`, `start` (`kind`), `continue`, `skip`, `cancel`, `apply`, `discard` | text | Config RPC thread (client), GUI RPC server thread (server) |
| C3 | Config editor, page: `createCalibrationPage` (`ConfigEditorWindow.cpp` 16709, registered at 5441), `runCalibrationCommand`, `applyCalibrationStatus`, `calibrationRpcThread_` | **(design)** P7 (doc 60 section 2): `output/calibration_lut/config_ui/CalibrationPage.{h,cpp}`, registered through H3 in the master VP-V page; thread joined in `OnEditorClosing` | document + RPC client | rows and status | Config UI thread |
| C4 | Config editor, Colour page rows: `refreshCalibrationControls` (3779), `liveCalibration*` members (3515), `setCalibrationStatusForTesting` (3765) | **(design)** R22 named anchors + R24 `OnDocumentEdited(section, key)` (doc 60) | live status (`ActiveProfileStatus`), edited key | enable/disable and status text of the LUT rows | Config UI thread |
| C5 | Config editor, load: `migrateCalibrationProfiles` (2560, 4842) -> `ConfigEditorCore::MigrateCalibrationProfiles` | **(design)** R24 `BeforeRebuild()`; migration plan stays in `CalibrationProfileMigration.h` (see README, "References that stay") | document | rewritten sections | Config UI thread |
| C6 | Schema: `MainConfigSchema.h` 64 (`Calibration::OwnsSection`) and 436 (`Calibration::ValidateConfig`); `RendererProfileConfig.h` key validators | stays until the SCHEMA lock work (W1-P3) lands; then the schema asks the module for its key list (**proposed** `CalibrationKeys.h`) | config | validation error text | Any thread that loads configuration |

Outputs of a finished run (what the wizard changes): only `calibration_lut_enabled` and the
`calibration_lut_<slot>` keys of the live Colour / Output profile, with byte backups first
(`<cfg>.before-calibration.<stamp>.bak`). Nothing else in the configuration is touched.

## 3. Config keys the module reads

`[vprenderer.color.<name>]` (and the base): `calibration_lut_enabled`, `calibration_lut_bt709`,
`calibration_lut_p3_d65`, `calibration_lut_bt2020`, `calibration_lut_input_transfer`, `hdr_tone_map_target_gamma`,
`sdr_lut_input_gamma`. Global: `calibration_lut_stage` (`libplacebo` | `vp`, read from `[vpv]` first), slot switch
`pipeline_slots_off` containing `output.calibration_lut`. Planned: `calibration_lut_reload`. The wizard's own
`[calibration]` section is defined by `CalibrationConfig.h` (beside the session code).

## 4. Failure and fallback entries

| Id | Engaged by | Meaning |
|---|---|---|
| `calibration LUT (the VP stage failed; ...)` | `AfterRender` | the VP hook failed or libplacebo disabled it; the LUT runs as libplacebo's target LUT for the renderer instance |
| `calibration LUT (dropped after a render error: <file>; ...)` | `LutDropped` | a render with the target LUT failed; Bill's reject dropped the LUT |
| `calibration patch (full field (window unavailable: <why>); ...)` | `Present` | windowed patch shown full field; released when patterns end |

## 5. Test seams (what a developer can run without the renderer)

- `IPatternSink`, `IMeter`, `ITerminal`, `ISessionClock` are interfaces: the session runs against
  `SimulatedScreen` (an `IPatternSink`, `CalibrationSimulation.h`), `SimulatedScreenMeter` and a scripted
  terminal (`tests/CalibrationTestSupport.h`) in `tests/` (CPU only). `CalibrationController::Hooks` replaces the
  renderer, meter, clock, projector and sensor for tests.
- `CalibrationLutHostPolicy.h` holds every decision the adapter makes (stage choice, status text, reload policy,
  output facts, fallback entries) as pure functions, tested by `CalibrationLutHostPolicyTests`.
- `pattern_lib/` has no dependencies at all (see its README).
- The only test that needs the real renderer and a GPU is
  `RendererCaptureHarnessTests::CalibrationDryRunThroughTheRenderer` (and the two `CalibrationPatterns...` /
  `CalibrationToneCurve...` cases beside it): see the README, "Running CalibrationDryRun".
