# Projector calibration LUT (`output.calibration_lut`)

Unit: **output** | side: output | order: 250 | status: **partial** | hook point: PRE_OUTPUT

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
- BTB/WTW (ADR-0002): the `vp` stage maps the clamped 0..1 point and adds the excess back unchanged (slope 1). The `libplacebo` stage clamps; ExtendedRange's late restore extrapolates along the LUT's edge slope (`extended_range_more_cases`). The two differ only on BTB/WTW codes.
- The Original pathway (`pipeline_original_pathway on`) always uses the `libplacebo` stage.
- Calibration patches bypass this slot: the renderer writes exact codes into the backbuffer (no LUT, no dither).

## Settings

- Color / Output profile keys (`[vprenderer.color.<name>]`; also the `[vprenderer]` base): `calibration_lut_enabled` (default false), `calibration_lut_bt709`, `calibration_lut_p3_d65`, `calibration_lut_bt2020` (a path relative to the configuration, or `none`). `sdr_target_primaries` picks the slot; `hdr_tone_map_target_gamma` and `sdr_lut_input_gamma` are part of the LUT's input contract.
- Compatibility keys: `calibration_lut_input_transfer` (accepted alias for `hdr_tone_map_target_gamma`); `calibration_lut_input_gamma` (retired: accepted and logged, no effect).
- `calibration_lut_stage` = `libplacebo` | `vp` (default `libplacebo`): implemented, global, read from `[vpv]` first, then `[vprenderer]`. Config: VP-V page, Expert. An invalid value is refused under "Fallbacks engaged" (`pipeline settings`).
- `pipeline_slots_off` containing `output.calibration_lut` stops the VP stage only; the LUT then runs as libplacebo's target LUT (the LUT is not switched off).
- There is no key named `calibration_lut` (slots.json lists one; it is only the renderer's change label).
- Automatic calibration: the `[calibration]` section (see "How the code works").
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entry (implemented): `calibration LUT (the VP stage failed; libplacebo's target LUT (before the LED compensation))`. It engages when the VP hook fails or libplacebo disables it, and lasts for the renderer instance. The slots.json wording `Calibration LUT: libplacebo target LUT (VP stage unavailable)` is not used by the code.
- Behaviour: the LUT runs as libplacebo's target LUT (the old order).
- Status only, no "Fallbacks engaged" entry (Ctrl+I `LUT:` line, Config's Color / Output page): LUT file rejected (`Rejected: <reason>`, no LUT); no LUT for the gamut (`Inactive: <reason>`); a changed file that fails to reload keeps the last good LUT (`Active (reload rejected: ...)`); a libplacebo render failure with the target LUT drops the LUT for the renderer instance (`Rejected: render error`).

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

Chain tests:
- BTB/WTW probes with the LUT: the GPU cases in `LibplaceboLutParserTests` (libplacebo stage only)

## Known issues / open items

- The move into VP's LAST output hook (pl_shader_custom_lut + BTB/WTW carried; barrier R1; owner agreed) is built behind `calibration_lut_stage = vp`; the default is still `libplacebo`. Open: a GPU test of the hook, the OutputEncodingRepair check, and switching the default after measurement (LED measurement then sees target light, not projector codes).

## How the code works

### a) The LUT at render time

Files: `CalibrationLutStage.h/.cpp` (this folder); `LibplaceboDisplayLut.h/.cpp` and `LibplaceboCalibrationLutPolicy.h` (src/VideoProcessor-Lib/vprenderer); caller `LibplaceboVideoRenderer.cpp`.

- Load: the renderer's `LoadDisplayLut` picks the slot with `LibplaceboCalibrationLut::Select` and parses the `.cube` with `LibplaceboDisplayLut::Load` (3D only, at most 65^3 and 64 MB, the path must stay under the configuration's base folder). The result is one `pl_custom_lut`.
- Reload: `MaybeReloadDisplayLut` checks the file version once a second and reloads transactionally. A failed reload with the same input contract keeps the last good LUT.
- Per frame: `ConfigureDisplayLutForTarget` either sets `target.lut` (libplacebo stage) or calls `CalibrationLutStage::Hook::SetLut` (VP stage). The VP stage needs `calibration_lut_stage = vp`, the Original pathway off, the slot running, and no earlier failure.
- The hook: `Hook::Apply` samples the PRE_OUTPUT texture (nearest), keeps the part outside 0..1 in a shader global, clamps, runs `pl_shader_custom_lut`, adds the excess back, and returns a new texture of the same size. It counts `Applied()` and `Failures()`.
- The renderer pushes the hook after the LED compensation and the user shaders and tags it with the slot id; `OutputHookOrder::OrderSlotsInPlace` keeps the slot order.
- Failure: after each render the renderer checks `Failures()` and libplacebo's disabled hooks. On a failure it sets `calibrationLutStageFailed` and engages the fallback above.
- GPU resources: `Hook::Release` frees the LUT shader object with the renderer's other GPU resources.
- Threading: all of this runs on the render thread. No locks.

### b) Automatic calibration (`calibration/`)

Purpose: a wizard in Config drives VP to show exact patches, reads a meter, fits a display model, and writes three `.cube` LUTs plus key-only configuration edits. Phase 2 adds the Dynamic Drive (LED) characterisation, the ambient-light check and the HDR (PQ) verify. Design: docs/VP-auto-calibration-design.md, docs/calibration/AUTO-CALIBRATION.md.

Public API:
- `CalibrationController` (CalibrationController.h): `Handle(command, arguments)` (text in, text out), `RendererGoing()`, `Shutdown()`, `Active()`. Commands: `status`, `detect`, `start` (`kind: calibration | led | ambient | hdr_verify`), `continue`, `skip`, `cancel`, `apply`, `discard`. Owned by `CVideoProcessorDlg` (src/VideoProcessor-GUI/VideoProcessorDlg.cpp, `HandleCalibrationRpc`) and reached over the configuration RPC (`Operation::Calibration`).
- `ICalibrationRun` (CalibrationSession.h): `CalibrationSession` (the LUT run); `MeterRun` (CalibrationRunBase.h) with `LedCharacterizationRun` (LedRun.h), `AmbientCheckRun` (AmbientCheck.h) and `HdrVerifyRun` (HdrVerify.h).
- `IPatternSink` (CalibrationSession.h): `RendererPatternSink` wraps `IVideoRenderer::GetCalibrationOutput` / `SetCalibrationPattern` (src/VideoProcessor-Lib/IRenderer.h). The patch layout is CalibrationPattern.h (POD; it crosses the renderer DLL boundary).
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
- The thread that serves the RPC calls `Handle`. It answers at once, but `start` does its setup there (config read, spotread search, meter and session construction).
- Session worker (`CalibrationController::worker`): calls `Step()` until the run finishes; one bounded unit per step (one patch, one reading, one second of warm-up). It cancels the run when Config has not polled for 60 s during a measuring step.
- Probe thread: `detect` runs `spotread -?` (`ProbeArgyll`, 20 s bound).
- spotread runs as a child process under a pseudo-console (ConPTY) inside a job object. `IMeter::Abort` (any thread) ends a wait in progress.
- Render thread: presents the patch (`PresentCalibrationPattern`); the sink calls only store the request under a mutex. While patterns show, the video is paused and the renderer stops VP's Dynamic Drive at full light.
- Each run keeps its state behind one mutex; commands set a pending command that the worker takes.

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
- Windowed patch impossible on the surface: the renderer shows full field and only logs it.

External tools:
- ArgyllCMS `spotread.exe` (AGPLv3; a separate process, never linked). Search order: `argyll_path`, `ARGYLL_PATH`, `PATH`, then `Argyll*` folders under the usual install roots (ArgyllInstall.h). Argyll 3.0 or newer is recommended.
- Any meter Argyll supports. MeterCatalog.h only tunes sensor calibration, CCSS/CCMX and the low-light floor per family. `meter_model: simulated` = a dry run with the real renderer and a simulated display and meter.
- Phase 2: the projector's RS-232 port (Barco F35 / FL35) and an optional fast light sensor on its own COM port.

Dependencies needed to lift it:
- Windows (ConPTY, job objects, `MultiByteToWideChar`), the project's `pch.h`, `DebugLog`.
- VideoProcessor-Lib: `ConfigFile.h`, `ConfigSchema.h`, `DisplayRuleExpression.h`, `DisplayProfile.h`, `AmbientConfig.h`, `IRenderer.h`, `led/LedDriveConfig.h`.
- VideoProcessor-ConfigCore: `ConfigEditorCore.h` (key-only edits, validation, `SaveSafely`).
- VP-V: `modules/drive/projector_control/ProjectorDriver.h` and `SerialPort.h` (LED run).
- The pure core (ColorMath.h, PatternPlan, PatternCodes, DisplayModel, LedCharacterize, the Spotread text parser, Simulated*) needs only the C++ standard library.

### Test it standalone

Build x64 Release (the AGENTS.md command), including `VideoProcessor-FakeSpotread`, which writes `x64\Release\test-tools\fake_spotread.exe`. Run tests single-threaded and read the TRX for the summary:

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~<Class>" /Logger:"trx;LogFileName=x.trx"`

- CPU, no hardware: `CalibrationCoreTests`, `CalibrationSpotreadTextTests`, `CalibrationMeterStateTests`, `CalibrationPatternTests`, `CalibrationSessionTests`, `CalibrationConfigTests`, `CalibrationPhase2Tests` (src/VideoProcessor-Test/Calibration*Tests.cpp; helpers in CalibrationTestSupport.h; scripted terminals replay src/VideoProcessor-Test/data/spotread).
- Real ConPTY with the fake meter: `CalibrationMeterTests`, `CalibrationTranscriptConPtyTests` (need fake_spotread.exe).
- Hardware, skipped unless set: `CalibrationHardwareTests` and `CalibrationTranscriptHardwareTests` (`VP_TEST_METER=1`; optional `VP_TEST_ARGYLL`, `VP_TEST_METER_PORT`, `VP_TEST_METER_CALIBRATED=1`); `CalibrationLedHardwareTests` (`VP_TEST_PROJECTOR=COMn`, VP not running).
- Render-time LUT: `LibplaceboLutParserTests` (GPU cases only with `VP_TEST_GPU=hardware`), `LibplaceboOutputPolicyTests`, `ExtendedRangeCarryTests`, `PipelineSkeletonTests`.

## Where the code lives (live file list)

Code location: **partly migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/output/calibration_lut/CalibrationLutStage.h`
- `src/VP-V/modules/output/calibration_lut/CalibrationLutStage.cpp`
- `src/VP-V/modules/output/calibration_lut/calibration`
- `src/VideoProcessor-Lib/vprenderer/LibplaceboDisplayLut.cpp`
- `src/VideoProcessor-Lib/vprenderer/LibplaceboDisplayLut.h`
- `src/VideoProcessor-Lib/vprenderer/LibplaceboCalibrationLutPolicy.h`

Forwarding headers at the legacy path (early migration, removed in M2; they only keep files on active branches building, new code includes the `<VP-V/...>` path):

- `src/VideoProcessor-Lib/calibration/CalibrationConfig.h`
- `src/VideoProcessor-Lib/calibration/CalibrationController.h`
- `src/VideoProcessor-Lib/calibration/CalibrationPattern.h`
- `src/VideoProcessor-Lib/calibration/CalibrationSimulation.h`
- `src/VideoProcessor-Lib/calibration/PatternCodes.h`

The automatic-calibration code (meter, ArgyllCMS, session, patterns; `calibration/`) moved here early. `LibplaceboDisplayLut.*` and `LibplaceboCalibrationLutPolicy.h` stay at their legacy paths for now (`VideoProcessor-Test.vcxproj`, changed by active branches, compiles `LibplaceboDisplayLut.cpp`; the renderer includes both).

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0013 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `calibration_lut`, `calibration_lut_enabled`, `calibration_lut_input_gamma`, `calibration_lut_input_transfer`.
- Planned keys (not yet implemented): `calibration_lut_stage`.
- Tiers: not tiered.
- VP-V page, Output tab: tiers Classic / Low; tier keys `temporal_dither`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#8-output](../../../../docs/settings/VP-settings-reference.md#8-output).
- Fallback entry ("Fallbacks engaged"): `Calibration LUT: libplacebo target LUT (VP stage unavailable)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): byte-identical while the VP stage is off (libplacebo LUT path unchanged).
- Isolation (fixed inputs, expected outputs, cost): LibplaceboDisplayLut tests.
- Contract (formats, geometry, fallback entry): applied after LED compensation.
- Chain (total delay at the 4/3/2-frame budgets): BTB/WTW probes with the LUT.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
