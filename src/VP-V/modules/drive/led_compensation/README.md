# LED compensation (adaptive gamma) (`drive.led_compensation`)

Unit: **drive** | side: output | order: 230 | status: **built** | hook point: PRE_OUTPUT

## Purpose / why

The Dynamic Drive control side: the core (`LedDriveModule`), the controllers that decide the LED/CNTR commands (`LedController`), the drive's config schema (`LedDriveConfig.h`), the look-ahead's raw-to-output transfer (`LedRawTransfer`), the between-files hold (`LedDriveHoldover`) and, since WP-15, the Dynamic Drive host adapter (`DynamicDriveHost` / `DynamicDriveShared` / `DynamicDriveFrame`) that holds all the LED glue the renderer host used to carry. The module runs the controllers and the projector driver, and gives each frame a gain. `DynamicDriveFrame` applies that gain with shaders/LED compensation.glsl at PRE_OUTPUT so content light stays constant while the LED (and CNTR) move; CNTR never above need. The same hook also carries the gamma boost (even with the drive off), the ANSI shadow lift (veil) and the script-mode CNTR smoothing. The port mutex (`LedDriveArbiter`) moved to `drive.projector_control` (transport).

## Contract

- Input: target-light picture at PRE_OUTPUT (after `drive.led_measure`'s hook), plus the frame's `CompensationGain` from `LedDriveModule::BeginFrame`
- Output: compensated picture. Compensate: a linear-light gain = flux(ceiling) / flux(modelled LED) / modelled CNTR light, limited by the highlight cap and knee. Script with `script_smoothing`: an encoded gain (at most 1) bridging whole CNTR steps. Below-black and above-white pass through (`led_btb_passthrough`, `led_wtw_passthrough`)
- Geometry: output size (the PRE_OUTPUT texture), whole picture
- Delay share: no picture delay. The gain follows the modelled LED: a send lands `led_latency_ms` after the driver wrote it (`led_landing_ms` on; else in whole frames), then settles with `led_settle_ms`; CNTR the same with `cntr_latency_ms` / `cntr_settle_ms`. With render-ahead the frame's time is its predicted display time (`BeginFrameAhead`)
- Look-ahead: compensate aims each command at the predicted stats of the frame it lands on (latency in source frames, from `drive.led_measure`'s predictions). When the look-ahead covers that frame the floor is `led_floor`, otherwise also `led_floor_reactive`. No queue (Low Latency): the latest exact stats

## How the code works

### Public API

- `LedDriveModule` (LedDriveModule.h):
  - Lifetime: constructor (`ProjectorConfig`, `LedDriveConfig`, port factory, clock), `Start`, `Stop` (restores full light; safe for concurrent callers), `Reconfigure` (live settings; returns false when a new module is needed: connection changed, or driving starts or stops).
  - Per frame (render thread): `BeginFrame` / `BeginFrameAhead` / `BeginFrameAt` return the `CompensationGain`; `SubmitMeasurement` takes the exact stats; `SetDisplayGamma`, `SetSourceRange`, `SetRawTransfer`.
  - Look-ahead: `Lookahead()` (the observer to subscribe to the queued-frame walk), `WantsPredictions`, `WantedLookaheadFrames`, `SubmitPrediction`, `ResetLookahead`.
  - Intent limits: `SetIntentLimits` (LED floor from `intent_black: led_protect` and ambient adaptation).
  - Diagnostics: `ProjectorStatus`, `DrivingProjector`, `CurrentGain`, `ModelledLight`, `AssistContrast`, `Recovering`, `MsSinceLastFrame`, `Config`.
  - Compensation availability (module findings item 1): `SetCompensationAvailable(available, reason)`, `CompensationAvailable`, `HoldingFullForCompensation`, `CompensationUnavailableReason(slotOn, shaderLoaded, shaderEnabled)`; `FallbackEntries()` (`kHoldFallbackId` "LED drive", `kDryRunFallbackId` "projector").
  - `IPredictionSink` (`drive.led_measure`, LedLookahead.h): the module is the look-ahead's sink.
- `CompensationGain`: `active`, `gain`, `domain` (encoded / linear), `knee`, `eotf`, `veil`. The renderer copies these, plus the gamma boost and passthrough flags, into the hook parameters (`gain`, `knee`, `gain_domain`, `compensation_eotf`, `boost`, `boost_knee`, `boost_knee_width`, `shadow_lift`, `veil`, `wtw_passthrough`, `btb_passthrough`).
- shaders/LED compensation.glsl: an mpv-style PREOUTPUT hook loaded by the renderer as rule `led_compensation`. It applies the boost first, then the shadow lift, then the gain with a soft knee below white; sign-preserving; negatives pass through when `btb_passthrough` is on.
- Controllers (LedController.h, pure, no I/O, no clock; moved here from `drive.projector_control` in the WP-15 re-cut): `ScriptController` (beta_22 port: light curve, step tables, CNTR bands), `PlanScriptLookahead`, `PaceScriptTick`, `CompensateController` (per-frame LED target, rate law, deadband, highlight protection), `CntrPairing` (CNTR assist, never above the need), `CutRecovery`, `FluxModel` (`flux_table`), `SettleModel`, and the gain helpers (`LedGain`, `CompensationGainFor*`).
- `LedDriveConfig.h`: `ProjectorConfig`, `LedDriveConfig`, `ProjectorRules` / `LedDriveRules`, `Read*`, `Validate`, the `[dynamic_drive.*]` alias, LED profiles, `BoostExponent`. (WP-18 may move the schema to `control.config`.)
- `BuildRawTransfer` / `RawTransferSignature` / `SampleToneCurve` (LedRawTransfer.*, libplacebo): the look-ahead's raw-to-output transfer from the frame's colour mapping; the tone-curve samples serve automatic calibration (phase 2).
- Dynamic Drive host adapter (WP-15; the renderer keeps one call per hook point, docs/design/wp15-led-callsites.md):
  - `DynamicDriveHost` (outer renderer, CPU): `ApplySnapshot` (UpdateLedDrive: settings signature, alias conflict, LED profile, validation, gamma-boost parameters, live `Reconfigure` or a new module through the arbitrated port factory or the hold-over), `Attach` (Build), `ParkForHoldover` (destructor), `SuspendForTest` / `ResumeAfterTest`, `StopForShutdown`, `SuspendForCalibration` / `ResumeAfterCalibration`, `StatusText`. It takes the renderer's state mutex by reference, with the old lock scopes.
  - `DynamicDriveShared` (no GPU types): what the outer host publishes to the render side: `PublishModule`, `PublishObserver`, `Clear`, `PublishParams`, `Module`, `WantedLookaheadFrames`.
  - `DynamicDriveFrame` (render thread, libplacebo; derives from `DynamicDriveShared`): `PrepareFrame(inputs, hooks, tagSlot)`, `OnHookErrors`, `CollectMeasurements`, `SendIntentLimits`, `MeasurementHookPointer`, `Release`; statics `PictureOnlyMasks`, `CompensationUnavailable`.
- The arbitrated port factory (`ArbitratedSerialPortFactory`, `drive.projector_control`) is what `DynamicDriveHost` hands a new module.
- `Holdover<Drive>` / `ProcessLedDriveHoldover()` / `EndHeldLedDrive` (LedDriveHoldover.h): in the filter process (`source=file`) the outgoing renderer parks its running drive for `file_led_holdover_ms`. The next renderer adopts it when its LED settings match; otherwise it is stopped (full light).

### Data flow

1. Settings apply: `DynamicDriveHost::ApplySnapshot` builds the module with the arbitrated port factory (or adopts a held one), starts it, and publishes it and its look-ahead observer through `DynamicDriveShared`.
2. Each frame: `BeginFrame` lands the driver's writes into the LED/CNTR models, steps `CompensateController` + `CntrPairing` + cut recovery + custom-dim fade (compensate), posts `SetLight` / `SetContrastAsync`, and returns the gain.
3. `DynamicDriveFrame::PrepareFrame` sets the hook parameters and pushes the hook after the measurement hook (if slot `drive.led_compensation` runs), then tells the module whether the compensation runs this frame (`SetCompensationAvailable`) and merges the module's fallback entries into the renderer's registry. After the render, `CollectMeasurements` feeds `SubmitMeasurement`.
4. Script mode decides on the module thread (`Run`) every `tick_ms`, or in the script's own pacing, and sends with `SendScript`.

### Threading

- Render thread: `BeginFrame*`, `SubmitMeasurement`, the look-ahead observer calls, `SetDisplayGamma` / `SetSourceRange` (atomics).
- Module thread (`Run`, started by `Start`): port open and handshake, script ticks, the compensate log (so the render thread never writes it), `led_idle_restore_s`, and the exit restore (`RestoreFullLight`) before the thread ends.
- One `mutex` guards the module state. Render-thread calls take it briefly and never wait on I/O: serial sends happen outside it, and compensate only posts asynchronous driver calls.
- Atomics: `compensating`, `applyGain`, `displayGamma`, `sourceRange`, `lookaheadEpoch`.
- `Reconfigure` and `SetIntentLimits` may come from any thread.
- The driver has its own worker thread (`drive.projector_control`), as does the arbiter. The holdover runs one process-wide thread that stops a parked drive at its deadline.
- Adapter: `DynamicDriveFrame` on the render thread (its `DynamicDriveShared` part is atomics, any thread); `DynamicDriveHost` on the settings-apply / GUI / RPC / shutdown threads, under the renderer's state mutex where the renderer took it before (`StopForShutdown` only try-locks).

### Config keys actually read

- `[led_drive.default]` (alias `[dynamic_drive.default]`; validated by `LedDriveRules` in LedDriveConfig.h, this folder):
  - Mode: `enabled` (off), `mode` (compensate | script | log_only | picture_only; `listen` retired), `command_source` (internal_script | compensate | none; `python` retired), `adaptive_gamma` (picture gain on/off; LED drive unaffected).
  - Look-ahead and timing: `lookahead` (on), `led_lookahead_ms` (0), `led_landing_ms` (on), `led_idle_restore_s` (10).
  - Controller: `led_floor` / `led_floor_reactive` / `led_floor_black` / `led_ceiling`, `rate_*`, `rate_preset`, `deadband`, `early_ramp`, `cut_jump`, `bright_cut_dump`, `highlight_tolerance_stops`, `highlight_area_small` / `_large`, `highlight_release_ms`.
  - CNTR assist: `cntr_assist` (on), `cntr_assist_max`, `software_first`, `cntr_pair_predict`, `cut_cntr_dump`, `cut_cntr_level`, `shaped_recovery`, `max_cut_dip`, `recovery_bias_pct`, `recovery_rate_*`, `black_prelift_max`, `preposition_black_only`, `cut_lead`.
  - Custom dim: `ftb_*`.
  - Correction: `correction_weight_by_time`.
  - Hook parameters: `knee` (0.08), `compensation_eotf` (2.41), `gamma_boost` (off), `gamma_boost_knee`, `gamma_boost_knee_width`, `shadow_lift_ansi` (only with `adaptive_gamma`) with `shadow_lift_ansi_ramp_lo` / `_hi` / `_rate`, `led_wtw_passthrough` (on), `led_btb_passthrough` (on).
  - `profile_file` (an LED profile merged under this section).
- `[projector.default]`: `led_latency_ms`, `led_settle_ms`, `cntr_latency_ms`, `cntr_settle_ms`, `cntr_max_usable`, `dmd_clip_cntr`, `flux_table`.
- `[general] file_led_holdover_ms` (3000; VP Renderer filter only).
- Renderer profile: `intent_black: led_protect` and ambient adaptation set the floor through `SetIntentLimits`.
- slots.json / MANIFEST list (WP-15): `led_drive` (the `[led_drive.default]` group), `adaptive_gamma`, `gamma_boost`, `shadow_lift_ansi`, `intent_black`. The old `led_compensation` (a shader rule name) is gone.

### What the user sees when it falls back

- With the WP-15 call-site patch applied (`DynamicDriveFrame`):
  - The shader does not load, libplacebo disables it, or slot `drive.led_compensation` is off while a module runs: compensate holds the LED at `led_ceiling` (a jump, not a ramp) and "Fallbacks engaged: LED drive (held at full: picture compensation unavailable (<reason>))" shows. Script mode is not held (its dimming is paired with CNTR; only `script_smoothing` is lost).
  - A configured gamma boost that cannot apply: "Fallbacks engaged: gamma boost (not applied: <reason>; the picture is plain output_gamma)" plus the log warning.
  - Dry run (no port, or the drive mutex held by another owner): "Fallbacks engaged: projector (not connected: <error>; LED drive off (dry run))"; no picture gain.
- Before that patch lands, the renderer's inline code still runs: none of the entries above is shown, and compensate keeps dimming without compensation (the module's default is "available").
- Invalid `[led_drive]` / `[projector]` settings: log "Dynamic Drive: configuration rejected (...); module and gamma boost off". An invalid LED profile is ignored with a log warning.
- Look-ahead starved (frames queued but no predictions): log "Dynamic Drive: WARNING - ... every cut and exit from black is reactive". Low Latency (no queue) is logged as reactive, not as a fault.
- Hold-over park, adopt and stop: log lines only ("LED drive: ... holding the LED drive for N ms", "adopted", "ending the held LED drive").

### Dependencies (to lift it)

- `drive.projector_control`: ProjectorDriver, SerialPort, LedClock, LedDriveArbiter (port factory).
- `drive.led_measure`: LedMeasurement, LedLookahead (`IPredictionSink`), FrameStats, LedMeasurementHook (the adapter).
- LedDriveConfig.h (here; pulls in ConfigFile.h, ConfigSchema.h, DisplayRuleExpression.h); DebugLog.h; Win32 (`GetTickCount64` in the holdover and the adapter).
- LedRawTransfer and DynamicDriveFrame need libplacebo and the renderer's helper types (CustomHookChain, SubtitleBoxDetector / SubtitleCutPaste, HdrPeakAnalysisCrop, NlsMaskMapping; Bill's headers are read, not owned). They compile in VideoProcessor-VPRenderer, -VPRendererBeta and -Test; DynamicDriveHost and the module compile in VideoProcessor-Lib.
- The shader file shaders/LED compensation.glsl, loaded through the renderer's `GlslHookLoader` (passed to `DynamicDriveFrame`). The module itself has no libplacebo dependency.
- Callers: LibplaceboVideoRenderer.cpp through the adapter (one call per hook point once the WP-15 patch is applied; today still the inline code), LibplaceboPluginExports.cpp (`EndHeldLedDrive`), VideoProcessorDlg.cpp (shutdown restore, connection test, through the renderer's IVideoRenderer calls).

### Test it standalone

- `LedDriveCompensateTests`, `LedDriveDelta1Tests` (compensate controller, CNTR pairing never above the need, cut recovery, flux table, config reading).
- `LedDriveHunt2Tests`, `LedDriveHunt3Tests` (resends, render-ahead landing, stream restarts, mode switches with the thread running).
- `LedIntentLimitsTests`.
- `LedDriveHoldoverTests`.
- `LedDriveControllerTests` (`ScriptController` against beta_22, tick for tick), `LedScriptCorrelationTests` (a no-op unless `VP_TEST_LED_CORRELATION=1` and `VP_TEST_REAL_FOOTAGE` are set).
- `DynamicDriveAdapterTests` (CPU): the host's settings / suspend / hold-over paths, the compensation-unavailable hold (compensate held, script not), the gamma-boost and dry-run entries, picture-only masks.
- `LedScriptLookaheadTests`.
- Shader (GPU, in `LibplaceboLutParserTests`): `LedCompensationShaderAppliesGainWithASoftKnee`, `LedCompensationKeepsBelowBlackAndLightExact`, `LedBoostToeAndShadowLift`. They run on WARP; `VP_TEST_GPU=hardware` uses the real GPU.
- Module tests use a fake port and `ManualClock` (LedDriveTestSupport.h); no projector needed.
- Run one class: `vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~LedDriveCompensateTests" /Logger:"trx;LogFileName=x.trx"`. Tests run single-threaded; read the TRX.

## Cost

Efficiency review 2026-10-24 (code reading only, nothing measured). Ranked by estimated saving.

1. **Identity compensation pass.** In log_only, dry run, script without smoothing, adaptive_gamma off, or any frame with gain 1, boost 1 and no lift, the hook still runs a full-screen PREOUTPUT pass that returns its input (the shader's own early-out). That is 2560x1600 x (8 B read + 8 B write) with an FP16 intermediate assumed, about 66 MB: roughly 0.07 ms on the 3090 and 0.35 ms on the 3050 Ti, double at a 4K output. Fix: do not push the hook when gain == 1, boost == 1 and shadow_lift x veil == 0. Byte-identical: yes while the measurement hook still runs (the PRE_OUTPUT intermediate stays); without it libplacebo may drop the FP16 intermediate, so not guaranteed. Risk low.
2. **Hook parameters through strings.** 11 `SetHookParameter` calls per frame. Each does `snprintf` "%.9g", builds a `std::map` node, scans the 11 parameters, runs `strtod` and returns a `std::vector`. `Ensure` also rebuilds 6 built-in strings and scans the ~8 KB shader for each token. Estimated 15-35 µs per frame on the render thread, at about 1-2 µs per call. Fix: resolve the `pl_hook_par` pointers once per hook; record at read time that the shader uses no built-ins. Byte-identical: yes if the "%.9g" + `strtod` conversion is kept (a direct double-to-float cast could differ in the last bit). Risk low.
3. **Compensate log built every step.** `StepCompensateLocked` builds an `ostringstream` (with `driver->Status()`) whenever a command goes out or no log is pending. The module thread prints it only when a command was sent or every 10 s. A few µs inside the module mutex. Fix: keep the fields; format them on the module thread when logged. Byte-identical: yes (picture; same log text). Risk low.
4. **`Config()` copies.** `LedDriveModule::Config()` returns the whole `LedDriveConfig` under the mutex (about 5 allocations per copy). The render side now takes one snapshot per frame (`DynamicDriveFrame::PrepareFrame`, was 4); the look-ahead still copies twice per observed frame. Fix the rest with accessors for the few fields. Byte-identical: yes. Risk low.
6. **Controller log string and flux scans (moved here with LedController).** `CompensateController::Tick` formats `out.log` with an `ostringstream` every frame (3-8 µs, render thread, inside the module mutex); `FluxModel::LevelFor` scans linearly. Fix: format only when logged; a 256-entry flux table per configuration. Byte-identical: yes. Risk low.
5. **Small per-frame allocations.** `LedLandingsLocked` allocates a vector (`cntr_pair_predict`). `Corrected()` copies a 1 KB histogram and re-bins it twice per step (plus once per queued frame while the custom dim checks for light ahead). A few µs. Fix: reuse buffers. Byte-identical: yes. Risk low.

When off: LED disabled, gamma_boost off and no shadow lift cost nothing (two atomic loads, `BoostExponent`). LED enabled but not compensating (log_only, dry run) costs the identity pass and the parameter writes above every frame.

## Settings

- Existing keys: `led_drive` (the `[led_drive.default]` group), `adaptive_gamma`, `gamma_boost`, `shadow_lift_ansi`, `intent_black`; details in "Config keys actually read" (plus `[projector.default]` timing keys and `[general] file_led_holdover_ms`).
- Planned keys: (none)
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entry: LED drive (held at full: picture compensation unavailable (<reason>)). Produced by the module, merged by `DynamicDriveFrame` (WP-15 patch). Related: "gamma boost (not applied: ...)".
- Behaviour: compensation unavailable -> compensate holds the LED at full drive (led_ceiling); script mode is not held.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `drive.led_measure`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA` (the shader tests run on WARP by default; `VP_TEST_GPU=hardware` for the real GPU).

Placeholder tests:
- (none yet)

Isolation tests:
- LED compensation shader tests (`LibplaceboLutParserTests`: `LedCompensation*`, `LedBoostToeAndShadowLift`)

Contract tests:
- CNTR never above need (`LedDriveDelta1Tests`, e.g. `CntrPairPredictNeverLandsAboveTheNeed`; `LedDriveCompensateTests`)

Chain tests:
- LED + Stage 13 + LUT + black level (no dedicated test found)

## Known issues / open items

- The hold and the entries need the WP-15 call-site patch (docs/design/wp15-led-callsites.md) applied by the integrator; until then the renderer's inline code runs without them.
- GPU checks owed (coordinator): shader missing / disabled on the real chain (LED held, entry shown), byte identity with the LED off and with compensate running.
- `LedDriveHoldover.*` is host lifetime (the filter's between-files hold); it stays here with the adapter that parks and adopts.

## Where the code lives (live file list)

Code location: **partly migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/drive/led_compensation/LedDriveModule.cpp`
- `src/VP-V/modules/drive/led_compensation/LedDriveModule.h`
- `src/VP-V/modules/drive/led_compensation/LedDriveHoldover.cpp`
- `src/VP-V/modules/drive/led_compensation/LedDriveHoldover.h`
- `src/VP-V/modules/drive/led_compensation/LedController.cpp`
- `src/VP-V/modules/drive/led_compensation/LedController.h`
- `src/VP-V/modules/drive/led_compensation/LedRawTransfer.cpp`
- `src/VP-V/modules/drive/led_compensation/LedRawTransfer.h`
- `src/VP-V/modules/drive/led_compensation/LedDriveConfig.h`
- `shaders/LED compensation.glsl`

Forwarding headers at the legacy path (early migration, removed in M2; they only keep files on active branches building, new code includes the `<VP-V/...>` path):

- `src/VP-V/modules/drive/led_compensation/LedDriveHoldover.h`
- `src/VP-V/modules/drive/led_compensation/LedDriveModule.h`
- `src/VP-V/modules/drive/led_compensation/LedRawTransfer.h`
- `src/VideoProcessor-Lib/led/LedDriveConfig.h`

`shaders/LED compensation.glsl` stays in the runtime shader folder.

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0013 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `adaptive_gamma`, `gamma_boost`, `intent_black`, `led_drive`, `shadow_lift_ansi`.
- Tiers: not tiered.
- VP-V page, Dynamic Drive tab: not tiered (Not a quality/cost choice: Dynamic Drive is set up on its own page (projector, script/compensate).).
- Plain-language guide: [docs/settings/VP-settings-reference.md#7-dynamic-drive](../../../../../docs/settings/VP-settings-reference.md#7-dynamic-drive).
- Fallback entry ("Fallbacks engaged"): `LED drive (held at full: picture compensation unavailable (<reason>))`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): LED compensation shader tests; LedController tests; Dynamic Drive adapter tests.
- Contract (formats, geometry, fallback entry): CNTR never above need; compensation unavailable holds full drive, visibly.
- Chain (total delay at the 4/3/2-frame budgets): LED + Stage 13 + LUT + black level.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
