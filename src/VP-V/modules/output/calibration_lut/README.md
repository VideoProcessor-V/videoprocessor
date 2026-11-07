# Projector calibration LUT (`output.calibration_lut`)

Unit: **output** | side: output | order: 250 | status: **partial** | hook point: PRE_OUTPUT

## Working on this module alone (start here)

Everything a developer needs to set up, change and bug-test the calibration module without the rest of VP's
design documents. The module is self-contained in this folder except for the Bill files listed in
[HOOKS.md](HOOKS.md) ("Renderer side", "Process and Config side"), which are the only places the rest of VP calls in.

### Layout

| Path | What it is | Depends on |
|---|---|---|
| `pattern_lib/` | The dependency-free pattern library: exact patch codes for 8-bit / 8-in-10 / 10-bit, patch plans, colour maths, reading policy, the POD structs shared with the renderer DLL. Own README, own stand-alone tests. | C++17 standard library only |
| `calibration/` | The automatic calibration: controller, session, meter (ArgyllCMS `spotread` over ConPTY), display model and LUT fit, outputs, LED characterisation, ambient check, HDR verify, simulated display and meter. | `pattern_lib/`, Windows (ConPTY, job objects), `pch.h`, `DebugLog`, VideoProcessor-Lib config classes, ConfigCore key-only edits, drive modules (LED run only) |
| `CalibrationLutStage.*` | The GPU hook that applies the `.cube` LUT as VP's last colour hook (`calibration_lut_stage = vp`). | libplacebo |
| `CalibrationLutHost.*`, `CalibrationLutHostPolicy.h` | The adapter between the renderer and everything above: patch presentation, LUT take/reload/attached/fallback decisions, output facts. The policy header is pure and CPU-testable. | libplacebo, D3D11 (adapter); none (policy) |
| `tests/` | MSTest classes compiled by VideoProcessor-Test, scripted spotread transcripts (`tests/data/spotread`). | see "Tests" below |
| `HOOKS.md` | The hook contract: every renderer and Config call site today and the hook it becomes. | - |

### Set up

1. A clone of `billslack2/videoprocessor`'s current beta integration branch (see `AGENTS.md`), Visual Studio 2022
   or newer with the C++ desktop workload, the MWCapture SDK headers (the build needs them even though this module
   never uses a capture card; point `MagewellSdkRoot` at `SDKv3`), and Qt as the rest of the solution requires.
2. Optional, for a real display: ArgyllCMS 3.0 or newer (`spotread.exe`) and a meter. Without them everything
   below still runs (simulated display and meter).

### Build (x64 Release, whole solution)

From a Visual Studio Developer PowerShell in the repository root (the `AGENTS.md` command, with your SDK path):

```
MSBuild.exe VideoProcessor.sln /t:Build /m /p:Configuration=Release /p:Platform=x64 /p:MagewellSdkRoot=<path to MWCaptureSDK>\SDKv3 /v:minimal
```

The build also produces `x64\Release\VideoProcessor-Test.dll` and `x64\Release\test-tools\fake_spotread.exe`
(project `src/VideoProcessor-FakeSpotread`, used by the ConPTY tests). If the linker reports LNK1103 or LNK1318 on
the renderer or GUI, delete the stale `.iobj` / `.ipdb` files under `x64\Release` and build again.

### Test it (fastest first)

1. Pattern library, no solution and no build needed (seconds):
   `powershell -NoProfile -ExecutionPolicy Bypass -File src\VP-V\modules\output\calibration_lut\pattern_lib\run_pattern_lib_tests.ps1 -OutDir <scratch folder>`
2. Everything CPU-only in this module, after the build above (a minute or two; no GPU, no meter). `vstest.console.exe`
   prints no summary for this host DLL, so always read the TRX `Counters` line:
   ```
   vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /Logger:"trx;LogFileName=calibration.trx" /ResultsDirectory:<scratch folder> /TestCaseFilter:"FullyQualifiedName~CalibrationCoreTests|FullyQualifiedName~CalibrationSpotreadTextTests|FullyQualifiedName~CalibrationMeterStateTests|FullyQualifiedName~CalibrationPatternTests|FullyQualifiedName~CalibrationSessionTests|FullyQualifiedName~CalibrationConfigTests|FullyQualifiedName~CalibrationPhase2Tests|FullyQualifiedName~CalibrationLutHostPolicyTests|FullyQualifiedName~CalibrationMeterTests|FullyQualifiedName~CalibrationTranscriptConPtyTests"
   ```
   The last two classes need ConPTY and `fake_spotread.exe` (built with the solution). Hardware classes
   (`CalibrationHardwareTests`, `CalibrationTranscriptHardwareTests`, `CalibrationLedHardwareTests`) skip unless
   their `VP_TEST_*` variables are set (see "Test it standalone" below).
3. The renderer, on a GPU: see "Running CalibrationDryRun".

### Running CalibrationDryRun

`RendererCaptureHarnessTests::CalibrationDryRunThroughTheRenderer` (`src/VP-V/tests/harness/RendererCaptureHarnessTests.cpp`)
is the end-to-end test: it starts the real VP renderer on the GPU with a synthetic 10-bit 4:2:2 signal, starts a
calibration session (Quick plan, `settle_ms` 40, no verify, 17-point LUT) at t = 1 s with a simulated meter that
reads the codes the renderer was asked to show, and expects the session to reach Review (more than 50 patches
measured) and the renderer to log `Calibration patterns: ended` within 28 s. It proves the patch timing (present
twice, then settle) and the end state (patterns off, video back) on a real swapchain.

Requirements: a desktop session with a real GPU (not a remote session), mains power, nothing else rendering, and
the display it opens on at its normal mode. Then, after the build:

```
set VP_TEST_GPU=hardware
set VP_TEST_ADAPTER=NVIDIA
vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /Logger:"trx;LogFileName=dryrun.trx" /ResultsDirectory:<scratch folder> /TestCaseFilter:"Name=CalibrationDryRunThroughTheRenderer"
```

(`VP_TEST_ADAPTER` is a name fragment of the adapter to use; leave it unset to use the default adapter.
`VP_TEST_REQUIRE_GPU=1` turns a skipped GPU test into a failure.) Read `dryrun.trx`: the test's standard output
has the line `dry run: <step> measured <n> skipped <n> format <...>` followed by the session message, and the
renderer's log lines (`Calibration patterns: started` / `ended`, `present failed`) are in the failure text.
Related GPU cases: `CalibrationPatternsAreExactCodesInTheBackbuffer` (reads the backbuffer back and compares codes)
and `CalibrationToneCurveReportedWithTheOutput`. In this team, GPU runs also follow the shared quiet-marker and
mains-only rules of the team's `TEAM-RULES.md`; an outside developer only needs a free GPU.

### References that stay outside the module (inventory and recommendation, W1-CAL 2026-11-07)

Two shared files mention the module's config keys. Neither is edited in this wave (`RendererProfileConfig.h` is
under the SCHEMA lock held by W1-P3; both are Bill's files). Recommendation: **keep both where they are**.

| File | References | What they are | Recommendation |
|---|---|---|---|
| `src/VideoProcessor-Lib/CalibrationProfileMigration.h` (about 180 lines; Bill's, extended by VP) | Included by `RendererProfileConfig.h` (line 9), `ConfigFile.cpp` (17; `Apply` at 404, the runtime load) and used by `ConfigEditorCore.cpp` (198, 210; `Build`, the comment-preserving editor). Tested in `ConfigFileTests.cpp` and `ConfigEditorCoreTests.cpp`. Holds the key list `IsKey` (`calibration_lut_enabled/bt709/p3_d65/bt2020`, `sdr_lut_input_gamma`, `hdr_tone_map_target_gamma`, `calibration_lut_input_transfer/gamma`), the archive section names, and the plan that moves those keys from Rendering profiles into Color profiles. | A profile-schema migration (which section owns which key), shared by the loader and the editor. It does not touch calibration logic. | Keep in `VideoProcessor-Lib`. It belongs with `ColorOutputProfileMigration.h` and the other migrations; moving it buys the module nothing and adds a Bill-file include change. Revisit only if the schema is ever generated from module manifests. |
| `src/VideoProcessor-Lib/RendererProfileConfig.h` | Value validators: `calibration_lut_enabled` (boolean), the three LUT paths (`none` or `*.cube`), `calibration_lut_input_gamma/transfer` and `hdr_tone_map_target_gamma` / `sdr_lut_input_gamma` (choices) (lines 977-996); `calibration_lut_stage` (choice `libplacebo`/`vp`, 1164; also in the global VP-V key list, 2186); rendering profiles may not carry calibration keys (1316); the Color profile's key set (1337-1339); alias and normalisation (`NormalizeCalibrationChoice`, 1450-1462); archive sections (62, 2207, 2677). | The schema's validation of the module's keys. | Keep until the SCHEMA work (W1-P3) lands. Then, in M2, give the module a small `CalibrationKeys.h` (key names, validators, defaults) and make the schema call it, so the key list lives in one place; that is a SCHEMA-lock change and belongs to that package, not this one. |

## Purpose / why

The LAST colour op: SDR target -> projector codes. The agreed place is VP's last PRE_OUTPUT colour hook, after LED compensation and black level (ADR-0013, barrier R1). Two paths exist, chosen by `calibration_lut_stage`:

- `libplacebo` (the default, byte-identical to before): the LUT is libplacebo's target LUT. It runs inside libplacebo's colour management, BEFORE VP's PRE_OUTPUT hooks, so LED measurement and compensation see projector codes (wrong order).
- `vp`: `CalibrationLutStage::Hook` applies the same LUT as VP's last colour hook (the agreed order). It stays off by default until the owner switches it after measurement.

The module also holds the automatic calibration (the `calibration/` sub-folder). It measures the display with a meter and writes the `.cube` LUTs this slot applies.

## Contract

- Input: final target-light picture (gamma-encoded target RGB in the profile's target gamut, before range encoding)
- Output: projector codes, same size and texture format
- Geometry: output size (the hook runs after the scaler)
- Delay share: 0 frames
- Look-ahead: none
- Hook point, `vp` stage: `PL_HOOK_PRE_OUTPUT`. Order (src/VP-V/core/OutputHookOrder.h): LED measurement (220) -> LED compensation (230) -> black level (240; placeholder, no hook yet) -> user PRE_OUTPUT shaders -> calibration LUT (250) -> de-judder rendered-picture cache -> OUTPUT-stage hooks -> output quantiser (sigma-delta).
- Hook point, `libplacebo` stage: libplacebo's target LUT (`pl_frame.lut`, `PL_LUT_NORMALIZED`), after tone/gamut mapping and before range encoding, so before every VP PRE_OUTPUT hook.
- LUT choice: one LUT per target gamut (`calibration_lut_bt709`, `calibration_lut_p3_d65`, `calibration_lut_bt2020`), picked by the profile's `sdr_target_primaries` (`LibplaceboCalibrationLut::Select`). No LUT for that gamut = no LUT.
- BTB/WTW (ADR-0002): the `vp` stage maps the clamped 0..1 point and adds the excess through the LUT's edge slope per axis (bug hunt 2026-10-31 F2; the same GLSL as ExtendedRange's late restore, `ExtendedRange::LutSlopeGlsl`). The `libplacebo` stage clamps; ExtendedRange's late restore extrapolates along the same edge slope (`extended_range_more_cases`). A LUT ExtendedRange cannot carry (shaper matrices, a side over 256) keeps slope 1 in the `vp` stage, counted in `SlopeFallbacks()`.
- The Original pathway (`pipeline_original_pathway on`) always uses the `libplacebo` stage.
- Calibration patches bypass this slot: the renderer writes exact codes into the backbuffer (no LUT, no dither).

## Settings

- Color / Output profile keys (`[vprenderer.color.<name>]`; also the `[vprenderer]` base): `calibration_lut_enabled` (default false), `calibration_lut_bt709`, `calibration_lut_p3_d65`, `calibration_lut_bt2020` (a path relative to the configuration, or `none`). `sdr_target_primaries` picks the slot; `hdr_tone_map_target_gamma` and `sdr_lut_input_gamma` are part of the LUT's input contract.
- Compatibility keys: `calibration_lut_input_transfer` (accepted alias for `hdr_tone_map_target_gamma`); `calibration_lut_input_gamma` (retired: accepted and logged, no effect).
- `calibration_lut_stage` = `libplacebo` | `vp` (default `libplacebo`): implemented, global, read from `[vpv]` first, then `[vprenderer]`. Config: VP-V page, Expert. An invalid value is refused under "Fallbacks engaged" (`pipeline settings`).
- `pipeline_slots_off` containing `output.calibration_lut` stops the VP stage only; the LUT then runs as libplacebo's target LUT (the LUT is not switched off).
- There is no key named `calibration_lut` (it is only the renderer's change label in its settings-change log).
- Planned (not read yet): `calibration_lut_reload` = `auto` | `apply`. `auto` is VP's reload (the selected LUT file is probed once a second and reloaded when it changes; today's behaviour); `apply` is Bill's upstream rule (files are read once per Config Apply), which arrives with the Bill merge. Either/or (owner rule); the Original pathway will select `apply`. VP's own calibration may load the LUT it has just written without a Config Apply, with a notice, in both modes. The policy is built and tested (`CalibrationLutHost::ReloadPolicy`); the key is wired with the Bill merge (WP-21) and the settings readers (WP-18).
- Automatic calibration: the `[calibration]` section (see "How the code works").
- Tiers: not tiered

## All settings (complete list)

Config placement: the master VP-V page (`calibration_lut_stage`, Expert) and Output > Calibration sub-page (the LUT rows and the wizard; the LUT rows are on the Colour / Output profile page today, the sub-page arrives with P7). "Original (Bill)" = what the Original pathway does with the key.

| Key | Section | Type | Range | Default | Config placement | Original (Bill) | Fallback / status |
|---|---|---|---|---|---|---|---|
| `calibration_lut_enabled` | `[vprenderer.color.<name>]` (and base) | bool | true/false | false | Output > Calibration | same (libplacebo target LUT) | invalid value: log + false; Ctrl+I `LUT:` line |
| `calibration_lut_bt709` / `_p3_d65` / `_bt2020` | same | path | file relative to the cfg, or `none` | empty (no LUT) | Output > Calibration | same | `Rejected: <reason>` / `Inactive: <reason>` status (not a downgrade) |
| `calibration_lut_input_transfer` | same (Rendering alias) | choice | as `hdr_tone_map_target_gamma` | empty | Colour / Output page | accepted alias | migration notice in the log |
| `hdr_tone_map_target_gamma` | same | choice | bt1886, srgb, 1.8..2.8 | 2.2 in the shipped cfg (empty resolves to 2.2) | Colour / Output page | same | none |
| `sdr_lut_input_gamma` | same | choice | passthrough, bt1886, srgb, 1.8..2.8 | passthrough | Colour / Output page | same | none |
| `calibration_lut_input_gamma` | same | retired | any | display | none | accepted, no effect | log line |
| `calibration_lut_stage` | `[vpv]`, then `[vprenderer]` (global) | choice | libplacebo, vp | libplacebo | master VP-V page, Expert | always libplacebo (Original pathway) | invalid: `pipeline settings` entry; VP stage failure: `calibration LUT (the VP stage failed; ...)` |
| `pipeline_slots_off` (entry `output.calibration_lut`) | `[vpv]` / `[vprenderer]` | list | slot ids | empty | master VP-V page | n/a | stops the VP stage only; LUT runs as libplacebo's target LUT |
| `calibration_lut_reload` (planned) | colour profile | choice | auto, apply | auto | Output > Calibration | apply | not read yet |

Tiers: not tiered. Patch display and the wizard have no settings of their own here (the wizard's `[calibration]` section is in `calibration/CalibrationConfig.h`); the patch fallback is `calibration patch (full field ...)`.

## Fallbacks

- "Fallbacks engaged" entries (implemented; each with the GUI's one-time notice, Ctrl+I, Config status and Ctrl+Alt+D):
  - `calibration LUT (the VP stage failed; libplacebo's target LUT (before the LED compensation))`: the VP hook failed or libplacebo disabled it (`Adapter::AfterRender`). The LUT then runs as libplacebo's target LUT (the old order) for the renderer instance.
  - `calibration LUT (dropped after a render error: <file>; no projector LUT for this renderer)`: a libplacebo render with the target LUT failed and Bill's `RejectDisplayLutAfterRenderFailure` dropped the LUT for the renderer instance (`Adapter::LutDropped`, WP-12; it used to be log-only).
  - `calibration patch (full field (window unavailable: <why>); LCD/OLED readings are affected)`: during a calibration run the surface could not take the windowed patch, so the patch is shown full field (`Adapter::Present`, WP-12; it used to be log-only). Released when the patterns end.
- Status only, no "Fallbacks engaged" entry (Ctrl+I `LUT:` line, Config's Color / Output page), because none is an automatic downgrade of a working LUT: LUT file rejected (`Rejected: <reason>`, no LUT); no LUT for the gamut (`Inactive: <reason>`); a changed file that fails to reload keeps the last good LUT (`Active (reload rejected: ...)`, or `Active (VP stage; reload rejected: ...)` while the VP stage applies it: WP-12 fix, the VP stage used to overwrite it every frame); after a render error `Rejected: render error` (plus the entry above).

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `drive.black_level`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_GPU=hardware`. Commands and test classes: "Test it standalone" below.

Placeholder tests:
- byte-identical while the VP stage is off (libplacebo LUT path unchanged): no dedicated test yet

Isolation tests:
- LibplaceboDisplayLut tests: `LibplaceboLutParserTests`; LUT policy: `LibplaceboOutputPolicyTests`; LUT edge extrapolation: `ExtendedRangeCarryTests`

Contract tests:
- applied after LED compensation: `PipelineSkeletonTests::OutputHooksTakeTheCorrectedOrder` (order only; no test runs `CalibrationLutStage::Hook` yet)
- the host adapter's decisions (stage choice, status text incl. a kept reload rejection, attached, reload policy both modes, output facts, the fallback entries): `CalibrationLutHostPolicyTests` (tests/CalibrationLutHostTests.cpp, CPU)

Chain tests:
- BTB/WTW probes with the LUT: the GPU cases in `LibplaceboLutParserTests` (libplacebo stage only)

## Known issues / open items

- The move into VP's LAST output hook (pl_shader_custom_lut + BTB/WTW carried; barrier R1; owner agreed) is built behind `calibration_lut_stage = vp`; the default is still `libplacebo`. Open: a GPU test of the hook, the OutputEncodingRepair check, and switching the default after measurement (LED measurement then sees target light, not projector codes).
- WP-12: the renderer side of the adapter is a patch for the integrator (docs/design/wp12-calib-callsites.patch); until it is applied the renderer still carries the old inline glue, and the WP-12 fixes in the adapter (kept reload rejection, attached with the VP stage, the two new fallback entries, the output-query wait) are not live.
- `calibration_lut_reload` (planned, see Settings): wire with the Bill merge.
- A windowed-patch fallback is visible as a "Fallbacks engaged" entry, but the session does not yet record it per patch: that needs a field in `PatternOutputInfo` (a plugin ABI change, "API: next"), left for the integrator.

## How the code works

### a) The LUT at render time

Files: `CalibrationLutStage.h/.cpp` (the hook), `CalibrationLutHost.h/.cpp` and `CalibrationLutHostPolicy.h` (the host adapter, WP-12) in this folder; Bill's `LibplaceboDisplayLut.h/.cpp` and `LibplaceboCalibrationLutPolicy.h` (src/VideoProcessor-Lib/vprenderer; not ours, never moved); caller `LibplaceboVideoRenderer.cpp`, one call per hook point into `CalibrationLutHost::Adapter` (docs/design/wp12-calib-callsites.md).

- Load: the renderer's `LoadDisplayLut` picks the slot with `LibplaceboCalibrationLut::Select` and parses the `.cube` with `LibplaceboDisplayLut::Load` (3D only, at most 65^3 and 64 MB, the path must stay under the configuration's base folder). The result is one `pl_custom_lut`.
- Reload: `MaybeReloadDisplayLut` asks the adapter (`LutReloadDue`, `ReloadPolicy` in auto mode) once per frame; the file version is probed once a second and a changed file is reloaded transactionally. A failed reload with the same input contract keeps the last good LUT. `LoadDisplayLut` reports the version it read (`NoteLutFileVersion`).
- Per frame: `ConfigureDisplayLutForTarget` calls `Adapter::TakeLut` first. It sets the stage's LUT (VP stage) or clears it; the VP stage needs `calibration_lut_stage = vp`, the Original pathway off, the slot running, and no earlier failure. Otherwise Bill's branch sets `target.lut` (libplacebo stage). `TakeLut` writes the VP-stage status only when it changes.
- The hook: `Hook::Apply` samples the PRE_OUTPUT texture (nearest), keeps the part outside 0..1 in a shader global, clamps, runs `pl_shader_custom_lut`, adds the excess back, and returns a new texture of the same size. It counts `Applied()` and `Failures()`.
- The renderer pushes `Adapter::HookForFrame()` after the LED compensation and the user shaders and tags it with the slot id; `OutputHookOrder::OrderSlotsInPlace` keeps the slot order. With the segment runner (WP-6) S2 pushes the same stage (`Adapter::Stage()`) as its last stage.
- Failure: after each render `Adapter::AfterRender` checks `Failures()` and libplacebo's disabled hooks. On a failure the stage stays off for the renderer instance and the renderer engages the fallback above. "Attached" (`Adapter::LutAttached`) counts the target LUT or the VP stage.
- GPU resources: `Hook::Release` frees the LUT shader object with the renderer's other GPU resources.
- Threading: all of this runs on the render thread. No locks.

### b) Automatic calibration (`calibration/`)

Purpose: a wizard in Config drives VP to show exact patches, reads a meter, fits a display model, and writes three `.cube` LUTs plus key-only configuration edits. Phase 2 adds the Dynamic Drive (LED) characterisation, the ambient-light check and the HDR (PQ) verify. Design: docs/VP-auto-calibration-design.md, docs/calibration/AUTO-CALIBRATION.md.

Public API:
- `CalibrationController` (CalibrationController.h): `Handle(command, arguments)` (text in, text out), `RendererGoing()`, `Shutdown()`, `Active()`. Commands: `status`, `detect`, `start` (`kind: calibration | led | ambient | hdr_verify`), `continue`, `skip`, `cancel`, `apply`, `discard`. Owned by `CVideoProcessorDlg` (src/VideoProcessor-GUI/VideoProcessorDlg.cpp, `HandleCalibrationRpc`) and reached over the configuration RPC (`Operation::Calibration`).
- `ICalibrationRun` (CalibrationSession.h): `CalibrationSession` (the LUT run); `MeterRun` (CalibrationRunBase.h) with `LedCharacterizationRun` (LedRun.h), `AmbientCheckRun` (AmbientCheck.h) and `HdrVerifyRun` (HdrVerify.h).
- `IPatternSink` (CalibrationSession.h): `RendererPatternSink` wraps `IVideoRenderer::GetCalibrationOutput` / `SetCalibrationPattern` (src/VideoProcessor-Lib/IRenderer.h). The patch layout is `pattern_lib/CalibrationPattern.h` (POD; it crosses the renderer DLL boundary).
- `IMeter` (Meter.h): `SpotreadMeter` (real), `SimulatedScreenMeter` (CalibrationSimulation.h, dry run).
- `ITerminal` (Terminal.h): `CreateConPtyTerminal` (ConPtyTerminal.cpp), `RecordingTerminal` (TranscriptRecorder.h).

Data flow (LUT run):
1. `start` reads `[calibration]` fresh from the configuration (`ReadSettings`), finds spotread (`FindSpotread`), and builds the meter and the session.
2. The session reads the output format from the sink (8-bit, 8-in-10, 10-bit; limited/full; SDR/PQ) and builds the plan (`BuildSessionPlan`, PatternPlan.h): white drift reference, greyscale incl. BTB/WTW, primaries/secondaries, cube, more drift references.
3. Each patch: integer codes (PatternCodes.h) -> `IPatternSink::Show` -> settle -> `IMeter::Read` (near-black reads averaged, patches below the meter's floor marked) -> one line in `raw.jsonl`.
4. Fitting (DisplayModel.h): black + 3x3 matrix + per-channel splines + residual field; Gauss-Newton inverse per target; a 3D LUT of `lut_size`; `.cube` files. The LUT's end cells follow ExtendedRange's edge-slope extrapolation.
5. Verify (`verify_after`): target-signal patches through the CPU LUT, dE2000 report.
6. Review shows the diff. Apply makes byte backups (`<cfg>.before-calibration.<stamp>.bak` and `<run>\before\`), then changes only `calibration_lut_enabled` and the `calibration_lut_<slot>` keys of the live Color / Output profile, validated and saved atomically (CalibrationRun.h, CalibrationOutputs.h). The controller then posts `WM_MESSAGE_CALIBRATION_APPLIED`; the GUI re-applies the saved configuration.
7. Outputs: `<vp>\luts\calibration\index.json` and `<vp>\luts\calibration\<display>\<timestamp>\` (`raw.jsonl`, `display_profile.cfg`, `calibration_<slot>.cube` x3, `verify.json`, `report.html`, `before\`).

Threading:
- The thread that serves the RPC calls `Handle`. It answers at once. `start` does only quick checks there (the kind, a fresh read and validation of the configuration, the LED run's protocol and port clashes) and returns the status `starting` (WP-12; it used to search for spotread and open the devices there).
- Session worker (`CalibrationController::RunWorker`): opens the projector, the fast sensor and the meter (the spotread search), builds the run and starts it; a failure there is the status (Failed) until the next start. Then it calls `Step()` until the run finishes; one bounded unit per step (one patch, one reading, one second of warm-up). It cancels the run when Config has not polled for 60 s during a measuring step. A cancel sent while the run is starting is applied when it starts.
- Probe thread: `detect` runs `spotread -?` (`ProbeArgyll`, 20 s bound).
- spotread runs as a child process under a pseudo-console (ConPTY) inside a job object. `IMeter::Abort` (any thread) ends a wait in progress.
- Render thread: presents the patch (`PresentCalibrationPattern` -> `CalibrationLutHost::Adapter::Present`); the sink calls only store the request under the adapter's mutex. While patterns show, the video is paused and the renderer stops VP's Dynamic Drive at full light. `GetCalibrationOutput` refreshes the facts under `renderMutex`: up to 100 try-locks 2 ms apart, but while patterns show and the cache is filled only one try-lock without sleeping (a busy renderer answers from the cache; a free one still refreshes, so a swapchain rebuilt mid-run is seen).
- Each run keeps its state behind one mutex; commands set a pending command that the worker takes. The controller's `worker`, `currentVpRoot` and start state are guarded by its `mutex`.

Config keys (`[calibration]`, CalibrationConfig.h; validated by `Calibration::ValidateConfig`, called from MainConfigSchema.h): `argyll_path`, `meter_model`, `meter_port`, `display_type`, `spectral_correction`, `refresh_mode`, `display_kind`, `display_name`, `plan`, `patch_window_percent`, `background_percent`, `settle_ms`, `near_black_max_reads`, `repeat_tolerance_percent`, `read_timeout_s`, `warmup_minutes`, `drift_interval`, `target_transfer`, `target_gamma`, `target_white_percent`, `lut_size`, `verify_after`; phase 2: `record_transcripts`, `transcript_folder`, `led_flux_step`, `led_dwell_ms`, `led_reads_per_level`, `led_cntr_sweep_max`, `led_cntr_dwell_ms`, `led_sensor_port`, `led_sensor_baud`, `led_step_repeats`, `hdr_verify_source_peak`, `hdr_verify_tone_curve`. The LED run also reads `[projector.default]` (protocol `barco_ascii`, port) and `[led_drive]`; the ambient check reads `[ambient]`.

What the user sees (wizard status in Config, plus the VP log; none is a "Fallbacks engaged" entry, because none is an automatic downgrade):
- spotread not found: `ArgyllInstallGuidance` (VP never ships or downloads Argyll).
- No ConPTY (Windows before 10 1809): the run is refused.
- No renderer, not rendering, or not the VP Renderer: the reason from `PatternOutputInfo::reason`.
- Renderer replaced (restart, new signal, exit): the run is cancelled.
- Config silent for 60 s while measuring: the run is cancelled.
- A `.ccss` file for a meter that takes only `.ccmx`: refused.
- Meter timeout: the spotread process is ended and reopened, never waited on forever.
- An interrupted LED run (VP crashed) leaves a marker; at the next start `RecoverInterruptedLedRun` restores full light, CNTR 50 and LDMM 0 before anything else opens the port.
- Windowed patch impossible on the surface: the renderer shows full field and engages `calibration patch` under "Fallbacks engaged" (WP-12; it was log-only). This one is an automatic downgrade, so it is the exception to the heading above.

External tools:
- ArgyllCMS `spotread.exe` (AGPLv3; a separate process, never linked). Search order: `argyll_path`, `ARGYLL_PATH`, `PATH`, then `Argyll*` folders under the usual install roots (ArgyllInstall.h). Argyll 3.0 or newer is recommended.
- Any meter Argyll supports. MeterCatalog.h only tunes sensor calibration, CCSS/CCMX and the low-light floor per family. `meter_model: simulated` = a dry run with the real renderer and a simulated display and meter.
- Phase 2: the projector's RS-232 port (Barco F35 / FL35) and an optional fast light sensor on its own COM port.

Dependencies needed to lift it:
- Windows (ConPTY, job objects, `MultiByteToWideChar`), the project's `pch.h`, `DebugLog`.
- VideoProcessor-Lib: `ConfigFile.h`, `ConfigSchema.h`, `DisplayRuleExpression.h`, `DisplayProfile.h`, `AmbientConfig.h` (forwarder; now in colour/appearance), `IRenderer.h`, `VP-V/modules/drive/led_compensation/LedDriveConfig.h`.
- VideoProcessor-ConfigCore: `ConfigEditorCore.h` (key-only edits, validation, `SaveSafely`).
- VP-V: `modules/drive/projector_control/ProjectorDriver.h` and `SerialPort.h` (LED run); `VP-V/modules/drive/led_compensation/LedRawTransfer.h` (the HDR-verify tone curve in `CalibrationLutHost`; WP-15 moves it to `modules/drive/led_compensation/`, switch the include at the rebase).
- The pure core needs only the C++ standard library: `pattern_lib/` (ColorMath, PatternPlan, PatternCodes, CalibrationPattern; its own stand-alone tests) and, in `calibration/`, DisplayModel, LedCharacterize, the Spotread text parser and Simulated*.

### Test it standalone

Build x64 Release (the AGENTS.md command), including `VideoProcessor-FakeSpotread`, which writes `x64\Release\test-tools\fake_spotread.exe`. Run tests single-threaded and read the TRX for the summary:

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~<Class>" /Logger:"trx;LogFileName=x.trx"`

- CPU, no hardware: `CalibrationCoreTests`, `CalibrationSpotreadTextTests`, `CalibrationMeterStateTests`, `CalibrationPatternTests`, `CalibrationSessionTests`, `CalibrationConfigTests`, `CalibrationPhase2Tests` (this module's `tests/Calibration*Tests.cpp`, compiled by VideoProcessor-Test; helpers in `tests/CalibrationTestSupport.h`; scripted terminals replay `tests/data/spotread`).
- Real ConPTY with the fake meter: `CalibrationMeterTests`, `CalibrationTranscriptConPtyTests` (need fake_spotread.exe).
- Hardware, skipped unless set: `CalibrationHardwareTests` and `CalibrationTranscriptHardwareTests` (`VP_TEST_METER=1`; optional `VP_TEST_ARGYLL`, `VP_TEST_METER_PORT`, `VP_TEST_METER_CALIBRATED=1`); `CalibrationLedHardwareTests` (`VP_TEST_PROJECTOR=COMn`, VP not running).
- Render-time LUT: `LibplaceboLutParserTests` (GPU cases only with `VP_TEST_GPU=hardware`), `LibplaceboOutputPolicyTests`, `ExtendedRangeCarryTests`, `PipelineSkeletonTests`.

## Cost

Reading-only review, 2026-10-24 (no builds, no runs; estimates, not measurements). Ranked by saving.

1. **Pattern-shown wait during a calibration run (wall time per patch).** Fixed in WP-12 (`FreshOutputAttempts`: one try-lock without sleeping while patterns show); the timing log confirmation is still open. `CalibrationSession::ShowPatch` (and `MeterRun`) poll `GetOutput` every 10 ms; `LibplaceboVideoRenderer::GetCalibrationOutput` try-locks `renderMutex` up to 100 times with `Sleep(2)` between tries, but the pattern loop holds `renderMutex` across `PresentCalibrationPattern` including the vsync-paced `Present` and re-locks at once. So almost every poll waits 100 x Sleep(2) before it falls back to the cache. How estimated: 100 tries x 2 ms (1 ms timer) to 15.6 ms (default timer) = about 0.2 to 1.6 s per patch; a few hundred patches = minutes per run. Fix: let the wait loop read only the generation atomics (a light query, or skip the locked refresh when patterns are active and the cache is filled). Byte-identical: yes (same codes; the settle still runs after the confirmation). Risk: low. Confirm the hold time with a timing log first.
2. **VP stage (`calibration_lut_stage = vp`): one extra full-frame pass on the GPU.** `Hook::Apply` reads the PRE_OUTPUT texture and writes a new same-size FBO texture (normally rgba16f); libplacebo's target LUT is fused into its colour pass at no extra pass. How estimated: 2560x1600 x 8 B read + write = 65 MB per frame, about 0.07 ms on the 3090 (936 GB/s) and 0.34 ms on the 3050 Ti (192 GB/s); 3840x2160 = 133 MB, about 0.14 / 0.7 ms; plus about 10-20 us CPU for the dispatch. The LUT fetches (4 texelFetch per pixel, tetrahedral) are the same in both paths. Fix: fold the clamp/LUT/restore into the shader of the hook directly before it (LED compensation) when the slots are adjacent. Byte-identical: no (one fp16 rounding fewer, at most 0.5 fp16 ulp, about 0.25 LSB at 10 bits, before the dither). Risk: medium (couples two slots; pipeline_order can move this slot).
3. **LUT load on the render thread (per load: renderer start, a live apply that changes a LUT key, a file change).** `LibplaceboDisplayLut::Load` reads the whole file, scans it for `LUT_1D_SIZE` (`ContainsOneDimensionalCubeDirective`), then `HasUnsupportedDomainDirective` copies the text into a `std::istringstream` and builds one more `std::istringstream` per line; then `pl_lut_parse_cube` hashes and parses it; the first frame builds the texture. How estimated: VP writes `%.7f %.7f %.7f` = 30 B per line; 65^3 = 274,625 lines = 8.2 MB, 33^3 (default) = 35,937 lines = 1.1 MB; one stream per line at about 0.5-1.5 us (MSVC locale set-up) = about 0.15-0.4 s for 65^3 and 20-50 ms for 33^3; the read, the 1D scan, the hash and libplacebo's float parse add about 30-60 ms for 65^3; the texture is RGBA32F, 65^3 x 16 B = 4.4 MB. Fix: one hand-written pass for both directive checks (no streams); also skip the re-read when only the input-contract key changed and the file version is unchanged; optionally parse on a worker and swap on the render thread. Byte-identical: yes (same bytes, same signature, same texture; a hand-written scan of the whole file keeps today's accept/reject exactly). Risk: low (worker parse: medium).
4. **Per-frame status strings.** VP stage fixed in WP-12 (written only on a change; the reload rejection kept); the libplacebo-stage label is Bill's code (a note for him). VP stage: `ConfigureDisplayLutForTarget` assigns `displayLutStatus = "Active (VP stage): " + LutFileName(...)` every frame (1-2 heap allocations; it also overwrites an "Active (reload rejected: ...)" status, so a rejected reload is not shown in the VP stage). libplacebo stage: builds an `std::ostringstream` label every frame only to compare it. How estimated: about 0.3-3 us per frame, under 0.2 ms per second at 60 fps. Fix: rebuild the label only when the LUT, path or stage changes. Byte-identical: yes. Risk: low (and fixes the hidden reload rejection).
5. **Pattern presentation per refresh (calibration only, video paused).** On VP's own swapchain each refresh does `GetBuffer`, `pl_d3d11_wrap`, clears (background, patch texture), a blit, `pl_gpu_flush` and `pl_tex_destroy`, although the patch is unchanged. How estimated: about 20-50 us CPU per refresh (texture wrap and views), GPU under 0.02 ms (one full-surface clear, 16 MB). Fix: keep the wrapped texture while the backbuffer is the same. Byte-identical: yes. Risk: low-medium (flip-model buffer rotation). The `PatternReadback` full-frame copy runs only for tests.
6. **File-version probe once a second.** `MaybeReloadDisplayLut` opens a handle and queries size and time on the render thread. How estimated: about 10-30 us per second on a local disk. Fix: `GetFileAttributesExA` (no handle) or a change notification on a worker. Byte-identical: yes. Risk: low.

Not a cost: the hook keeps the LUT texture in its `pl_shader_obj` and re-uploads only when the LUT signature changes; the 50 ms idle sleeps and the event-driven ConPTY reader cost nothing measurable.

When off (no LUT, or the libplacebo stage, or the slot off): no hook is pushed; per frame only `ConfigureDisplayLutForTarget`'s early return and a `GetTickCount64` compare in `MaybeReloadDisplayLut` (a few ns); with a LUT in the libplacebo stage, the fused LUT fetches in libplacebo's own pass plus item 4's label.

## Where the code lives (live file list)

Code location: **partly migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/output/calibration_lut/CalibrationLutStage.h`
- `src/VP-V/modules/output/calibration_lut/CalibrationLutStage.cpp`
- `src/VP-V/modules/output/calibration_lut/CalibrationLutHost.h`
- `src/VP-V/modules/output/calibration_lut/CalibrationLutHost.cpp`
- `src/VP-V/modules/output/calibration_lut/CalibrationLutHostPolicy.h`
- `src/VP-V/modules/output/calibration_lut/calibration`
- `src/VP-V/modules/output/calibration_lut/pattern_lib`
- `src/VideoProcessor-Lib/vprenderer/LibplaceboDisplayLut.cpp`
- `src/VideoProcessor-Lib/vprenderer/LibplaceboDisplayLut.h`
- `src/VideoProcessor-Lib/vprenderer/LibplaceboCalibrationLutPolicy.h`

Forwarding headers at the legacy path (early migration; they only keep files on active branches building, new code includes the `<VP-V/...>` path). They are NOT removed in this wave because Bill's files include them (W1-CAL, 2026-11-07), so they move with M2 P0/P3. Includers: `src/VideoProcessor-Lib/IRenderer.h` line 25 (the Pattern header), `src/VideoProcessor-Lib/MainConfigSchema.h` line 7 (the Config header; SCHEMA lock) and `src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp` line 138 (the Pattern header). No VP file includes them any more:

- `src/VideoProcessor-Lib/calibration/CalibrationConfig.h`
- `src/VideoProcessor-Lib/calibration/CalibrationPattern.h`

The automatic-calibration code (meter, ArgyllCMS, session, patterns; `calibration/`) moved here early; its tests moved to `tests/` in M2 step 3 (WP-12). `LibplaceboDisplayLut.*` and `LibplaceboCalibrationLutPolicy.h` are Bill's files (present in his upstream beta): they never move; the slot uses them (listed in `live_files`, not owned). The renderer's calibration glue moved into `CalibrationLutHost` (WP-12, option (d)); the forwarding headers go in M2 step 0.

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0013 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `calibration_lut_bt2020`, `calibration_lut_bt709`, `calibration_lut_enabled`, `calibration_lut_input_transfer`, `calibration_lut_p3_d65`, `calibration_lut_stage`.
- Planned keys (not yet implemented): `calibration_lut_reload`.
- Tiers: not tiered.
- VP-V page, Output tab: tiers Classic / Low / Mid; tier keys `temporal_dither`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#8-output](../../../../../docs/settings/VP-settings-reference.md#8-output).
- Fallback entry ("Fallbacks engaged"): `calibration LUT (the VP stage failed; libplacebo's target LUT (before the LED compensation))`.

## Tests

- Placeholder (byte-identity on/off, zero cost): byte-identical while the VP stage is off (libplacebo LUT path unchanged).
- Isolation (fixed inputs, expected outputs, cost): LibplaceboDisplayLut tests.
- Contract (formats, geometry, fallback entry): applied after LED compensation.
- Chain (total delay at the 4/3/2-frame budgets): BTB/WTW probes with the LUT.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
